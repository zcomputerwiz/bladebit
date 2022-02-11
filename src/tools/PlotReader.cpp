#include "PlotReader.h"
#include "ChiaConsts.h"
#include "util/BitView.h"
#include "plotshared/PlotTools.h"
#include "memplot/CTables.h"
#include "DTables.h"

///
/// Plot Reader
///

//-----------------------------------------------------------
PlotReader::PlotReader( IPlotFile& plot )
    : _plot( plot )
{
    const size_t largestParkSize           = RoundUpToNextBoundaryT( CalculateParkSize( TableId::Table1 ), sizeof( uint64 ) * 2 );
    const size_t maxDecompressedDeltasSize = RoundUpToNextBoundaryT( (size_t)0x7FFF, sizeof( uint64 ) );

    _parkBuffer   = bbmalloc<uint64>( largestParkSize );
    _deltasBuffer = bbmalloc<byte>  ( maxDecompressedDeltasSize );
}

//-----------------------------------------------------------
PlotReader::~PlotReader()
{
    free( _parkBuffer );
    free( _deltasBuffer );
}

//-----------------------------------------------------------
// uint64 PlotReader::GetC3ParkCount() const
// {
//     // We know how many C3 parks there are by how many 
//     // entries we have in the C1 table - 1 (extra 0 entry added)
//     // However, to make sure this is the case, we'll have to 
//     // read-in all C1 entries and ensure we hit an empty one,
//     // to ensure we don't run into dead/alignment-space
//     const size_t c1Size = _plot.TableSize( PlotTable::C1 );
// }

// //-----------------------------------------------------------
// uint64 PlotReader::GetF7EntryCount() const
// {
// }

//-----------------------------------------------------------
int64 PlotReader::ReadC3Park( uint64 parkIndex, uint64* f7Buffer )
{
    const uint32 k              = _plot.K();
    const size_t f7SizeBytes    = CDiv( k, 8 );
    const size_t c3ParkSize     = CalculateC3Size();
    const uint64 c1Address      = _plot.TableAddress( PlotTable::C1 );
    const uint64 c3Address      = _plot.TableAddress( PlotTable::C3 );
    const uint64 c1EntryAddress = c1Address + parkIndex * f7SizeBytes;
    const uint64 parkAddress    = c3Address + parkIndex * c3ParkSize;

    // First we need to read the root F7 entry for the park, 
    // which is at in C1 table.
    if( !_plot.Seek( SeekOrigin::Begin, (int64)c1EntryAddress ) )
        return -1;

    // #TODO: Ensure the C1 address is within the C1 table bounds.
    uint64 c1 = 0;
    if( _plot.Read( f7SizeBytes, &c1 ) != (ssize_t)f7SizeBytes )
        return -1;

    c1 = Swap64( c1 << ( 64 - k ) );

    // #TODO: Ensure we can read this park. If it's not present, it means
    //        the C1 entry is the only entry in the park, so just return it.
    // Read the park into our buffer
    if( !_plot.Seek( SeekOrigin::Begin, (int64)parkAddress ) )
        return -1;

    // Read the size of the compressed C3 deltas
    uint16 compressedSize = 0;
    if( _plot.Read( sizeof( uint16 ), &compressedSize ) != (ssize_t)sizeof( uint16 ) )
        return -1;

    compressedSize = Swap16( compressedSize );
    if( compressedSize > c3ParkSize )
        return -1;

    // memset( _parkBuffer, 0, _parkBufferSize );
    if( _plot.Read( c3ParkSize - sizeof( uint16 ), _parkBuffer ) != c3ParkSize - sizeof( uint16 ) )
        return -1;

    // Now we can read the f7 deltas from the C3 park
    const size_t deltaCount = FSE_decompress_usingDTable( 
                                _deltasBuffer, kCheckpoint1Interval, 
                                _parkBuffer, compressedSize, 
                                (const FSE_DTable*)DTable_C3 );

    if( FSE_isError( deltaCount ) )
        return -1;           // #TODO: Set error message locally

    ASSERT( deltaCount );
    for( uint32 i = 0; i < deltaCount; i++ )
        if( _deltasBuffer[i] == 0xFF )
            return -1;

    // Unpack deltas into absolute values
    memset( f7Buffer, 0, kCheckpoint1Interval * sizeof( uint64 ) );

    uint64 f7 = c1;
    f7Buffer[0] = f7;

    f7Buffer++;
    for( int32 i = 0; i < (int32)deltaCount; i++ )
        f7Buffer[i] = f7Buffer[i-1] + _deltasBuffer[i];

    return (int64)deltaCount+1;
}

//-----------------------------------------------------------
bool PlotReader::ReadP7Entries( uint64 parkIndex, uint64* p7Indices )
{
    const uint32 k              = _plot.K();
    const uint32 p7EntrySize    = k + 1;
    const uint64 p7TableAddress = _plot.TableAddress( PlotTable::Table7 );
    const size_t p7TableMaxSize = _plot.TableSize( PlotTable::Table7 );
    const size_t parkSizeBytes  = CalculatePark7Size( k );

    const uint64 maxParks       = p7TableMaxSize / parkSizeBytes;

    // Park must be in the range of the maximum table parks encoded
    if( parkIndex >= maxParks )
        return false;

    const uint64 parkAddress = p7TableAddress + parkIndex * parkSizeBytes;

    if( !_plot.Seek( SeekOrigin::Begin, (int64)parkAddress ) )
        return false;

    if( _plot.Read( parkSizeBytes, _parkBuffer ) != parkSizeBytes )
        return false;

    BitReader parkReader( _parkBuffer, parkSizeBytes * 8 );

    for( uint32 i = 0; i < kEntriesPerPark; i++ )
        p7Indices[i] = parkReader.ReadBits64( p7EntrySize );

    return true;
}

//-----------------------------------------------------------
bool PlotReader::ReadLPPark( PlotTable table, uint64 parkIndex, uint128 linePoints[kEntriesPerPark], uint64& outEntryCount )
{
    outEntryCount = 0;

    ASSERT( linePoints );
    ASSERT( table < PlotTable::C1 );

    if( table >= PlotTable::C1 )
        return false;

    const uint32 k              = _plot.K();
    const size_t lpSizeBytes    = LinePointSizeBytes( k );
    const size_t tableMaxSize   = _plot.TableSize( table );
    const size_t tableAddress   = _plot.TableAddress( table );
    const size_t parkSize       = CalculateParkSize( (TableId)table );

    const uint64 maxParks       = tableMaxSize / parkSize;
    if( parkIndex >= maxParks )
        return false;
    
    const size_t parkAddress    = tableAddress + parkIndex * parkSize;
    
    if( !_plot.Seek( SeekOrigin::Begin, (int64)parkAddress ) )
        return false;

    // Read base full line point
    uint128 baseLinePoint;
    {
        uint64 baseLPBytes[CDiv(LinePointSizeBytes( 50 ), 8)] = { 0 };

        if( _plot.Read( lpSizeBytes, baseLPBytes ) != lpSizeBytes )
            return false;

        const size_t lpSizeBits = LinePointSizeBytes( k ) * 8;
        BitReader lpReader( baseLPBytes, RoundUpToNextBoundary( lpSizeBits, 8 ) );
        baseLinePoint = lpReader.ReadBits128( lpSizeBits );
    }

    // Read stubs
    const size_t stubsSizeBytes = CDiv( ( kEntriesPerPark - 1 ) * ( k - kStubMinusBits ), 8 );
    uint64* stubsBuffer = _parkBuffer;

    if( _plot.Read( stubsSizeBytes, stubsBuffer ) != stubsSizeBytes )
        return false;

    // Read deltas
    const size_t maxDeltasSizeBytes = CalculateMaxDeltasSize( (TableId)table );
    byte* compressedDeltaBuffer = ((byte*)_parkBuffer) + RoundUpToNextBoundary( stubsSizeBytes, sizeof( uint64 ) );
    byte* deltaBuffer = _deltasBuffer;
    
    uint16 compressedDeltasSize = 0;
    if( _plot.Read( 2, &compressedDeltasSize ) != 2 )
        return false;

    if( !( compressedDeltasSize & 0x8000 ) && compressedDeltasSize > maxDeltasSizeBytes )
        return false;

    size_t deltaCount = 0;
    if( compressedDeltasSize & 0x8000 ) 
    {
        // Uncompressed
        compressedDeltasSize &= 0x7fff;
        if( _plot.Read( compressedDeltasSize, compressedDeltaBuffer ) != compressedDeltasSize )
            return false;

        deltaCount = compressedDeltasSize;
    }
    else
    {
        // Compressed
        if( _plot.Read( compressedDeltasSize, compressedDeltaBuffer ) != compressedDeltasSize )
            return false;

        // Decompress deltas
        deltaCount = FSE_decompress_usingDTable( 
                        deltaBuffer, kEntriesPerPark - 1, 
                        compressedDeltaBuffer, compressedDeltasSize, 
                        DTables[(int)table] );

        if( FSE_isError( deltaCount ) )
            return false;
    }

    // Decode line points from stubs and deltas
    linePoints[0] = baseLinePoint;
    if( deltaCount > 0 )
    {
        const uint32 stubBitSize = ( _K - kStubMinusBits );

        BitReader stubReader( stubsBuffer, RoundUpToNextBoundary( stubsSizeBytes * 8, 64 ) );
        
        for( uint64 i = 1; i <= deltaCount; i++ )
        {
            // Since these entries are still deltafied, we can fit them in 64-bits
            const uint64 stub = stubReader.ReadBits64( stubBitSize );
            const uint64 lp   = stub | (((uint64)deltaBuffer[i-1]) << stubBitSize );

            // Get absolute LP from delta
            linePoints[i] = linePoints[i-1] + (uint128)lp;
        }
    }


    outEntryCount = deltaCount + 1;
}

//-----------------------------------------------------------
bool PlotReader::FetchProofFromP7Entry( uint64 p7Entry, uint64 proof[32] )
{
    // Read linepoint

    // for( PlotTable table = PlotTable::Table6 )
}

///
/// Plot Files
///
// #TODO: Move to other source files
//-----------------------------------------------------------
bool IPlotFile::ReadHeader( int& error )
{
    error = 0;

    // Magic
    {
        char magic[sizeof( kPOSMagic )-1] = { 0 };
        if( Read( sizeof( magic ), magic ) != sizeof( magic ) )
            return false;
        
        if( !MemCmp( magic, kPOSMagic, sizeof( magic ) ) )
        {
            error -1;       // #TODO: Set actual user error
            return false;
        }
    }
    
    // Plot Id
    {
        if( Read( sizeof( _header.id ), _header.id ) != sizeof( _header.id ) )
            return false;

        // char str[65] = { 0 };
        // size_t numEncoded = 0;
        // BytesToHexStr( _header.id, sizeof( _header.id ), str, sizeof( str ), numEncoded );
        // ASSERT( numEncoded == sizeof( _header.id ) );
        // _idString = str;
    }

    // K
    {
        byte k = 0;
        if( Read( 1, &k ) != 1 )
            return false;

        _header.k = k;
    }

    // Format Descritption
    {
        const uint formatDescSize =  ReadUInt16();
        FatalIf( formatDescSize != sizeof( kFormatDescription ) - 1, "Invalid format description size." );

        char desc[sizeof( kFormatDescription )-1] = { 0 };
        if( Read( sizeof( desc ), desc ) != sizeof( desc ) )
            return false;
        
        if( !MemCmp( desc, kFormatDescription, sizeof( desc ) ) )
        {
            error = -1; // #TODO: Set proper user error
            return false;
        }
    }
    
    // Memo
    {
        uint memoSize = ReadUInt16();
        if( memoSize > sizeof( _header.memo ) )
        {
            error = -1; // #TODO: Set proper user error
            return false;
        } 

        _header.memoLength = memoSize;

        if( Read( memoSize, _header.memo ) != memoSize )
        {
            error = -1; // #TODO: Set proper user error
            return false;
        }

        // char str[BB_PLOT_MEMO_MAX_SIZE*2+1] = { 0 };
        // size_t numEncoded = 0;
        // BytesToHexStr( _memo, memoSize, str, sizeof( str ), numEncoded );
        
        // _memoString = str;
    }

    // Table pointers
    if( Read( sizeof( _header.tablePtrs ), _header.tablePtrs ) != sizeof( _header.tablePtrs ) )
    {
        error = -1; // #TODO: Set proper user error
        return false;
    }

    for( int i = 0; i < 10; i++ )
        _header.tablePtrs[i] = Swap64( _header.tablePtrs[i] );

    // What follows is table data
    return true;
}


///
/// Memory Plot
///
//-----------------------------------------------------------
MemoryPlot::MemoryPlot()
    : _bytes( nullptr, 0 )
{}

//-----------------------------------------------------------
MemoryPlot::~MemoryPlot()
{
    if( _bytes.values )
        SysHost::VirtualFree( _bytes.values );

    _bytes = Span<byte>( nullptr, 0 );
}

//-----------------------------------------------------------
bool MemoryPlot::Open( const char* path )
{
    ASSERT( path );
    if( !path )
        return false;

    if( IsOpen() )
        return false;

    FileStream file;    // #TODO: Enable no buffering again, for now when testing disable to take advantage of caching.
    if( !file.Open( path, FileMode::Open, FileAccess::Read ) )//, FileFlags::LargeFile | FileFlags::NoBuffering ) )
    {
        _err = file.GetError();
        return false;
    }

    const ssize_t plotSize = file.Size();
    if( plotSize <= 0 )
    {
        if( plotSize < 0 )
            _err = file.GetError();
        else
            _err = -1;  // #TODO: Assign an actual user error.
        return false;
    }

    // Add an extra block at the end to be able to do an aligned read there if
    // we have any remainder that does not align to a block
    const size_t allocSize = RoundUpToNextBoundary( (size_t)plotSize, (int)file.BlockSize() ) + file.BlockSize();

    byte* bytes = (byte*)SysHost::VirtualAlloc( allocSize );
    if( !bytes )
    {
        _err = -1;      // #TODO: Assign an actual user error.
        return false;
    }

    // Read the whole thing to memory
    size_t readSize      = RoundUpToNextBoundary( plotSize, (int)file.BlockSize() );/// file.BlockSize() * file.BlockSize();
    // size_t readRemainder = plotSize - readSize;
    const size_t readEnd = readSize - plotSize;
    byte*  reader        = bytes;
    
    // Read blocks
    while( readSize > readEnd )
    {
        const ssize_t read = file.Read( reader, readSize );
        ASSERT( read );

        if( read < 0 )
        {
            _err = file.GetError();
            SysHost::VirtualFree( bytes );

            return false;
        }

        readSize -= (size_t)read;
        reader   += read;
    }

    // if( readRemainder )
    // {
    //     byte* block = (byte*)RoundUpToNextBoundary( (uintptr_t)reader, (int)file.BlockSize() );

    //     const ssize_t read = file.Read( block, (size_t)file.BlockSize() );
    //     ASSERT( read );
    //     ASSERT( read >= readRemainder );

    //     if( read < 0 )
    //     {
    //         _err = file.GetError();
    //         SysHost::VirtualFree( bytes );

    //         return false;
    //     }

    //     if( reader != block )
    //         memmove( reader, block, readRemainder );
    // }

    _bytes = Span<byte>( bytes, (size_t)plotSize );

    // Read the header
    int headerError = 0;
    if( !ReadHeader( headerError ) )
    {
        if( headerError )
            _err = headerError;

        if( _err == 0 )
            _err = -1; // #TODO: Set generic plot header read error

        _bytes.values = nullptr;
        SysHost::VirtualFree( bytes );
        return false;
    }

    // Save data, good to go
    _plotPath = path;

    return true;
}

//-----------------------------------------------------------
bool MemoryPlot::IsOpen()
{
    return _bytes.values != nullptr;
}

//-----------------------------------------------------------
size_t MemoryPlot::PlotSize() const
{
    return _bytes.length;
}

//-----------------------------------------------------------
bool MemoryPlot::Seek( SeekOrigin origin, int64 offset )
{
    ssize_t absPosition = 0;

    switch( origin )
    {
        case SeekOrigin::Begin:
            absPosition = offset;
            break;

        case SeekOrigin::Current:
            absPosition = _position + offset;
            break;

        case SeekOrigin::End:
            absPosition = (ssize_t)_bytes.length + offset;
            break;
    
        default:
            _err =  -1;     // #TODO: Set proper user error.
            return false;
    }

    if( absPosition < 0 || absPosition > (ssize_t)_bytes.length )
    {
        _err =  -1;     // #TODO: Set proper user error.
        return false;
    }

    _position = absPosition;
    return true;
}

//-----------------------------------------------------------
ssize_t MemoryPlot::Read( size_t size, void* buffer )
{
    if( size < 1 || !buffer )
        return 0;

    ASSERT( buffer );

    const size_t endPos = (size_t)_position + size;

    if( endPos > _bytes.length )
    {
        _err = -1; // #TODO: Set proper user error
        return false;
    }

    memcpy( buffer, _bytes.values + _position, size );
    _position = (ssize_t)endPos;

    return (ssize_t)size;
}

//-----------------------------------------------------------
int MemoryPlot::GetError() 
{
    return _err;
}



