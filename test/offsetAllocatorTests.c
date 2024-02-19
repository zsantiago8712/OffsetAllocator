#include "../offsetAllocator.h"
#include "munit.h"

extern uint32 uintToFloatRoundUp(const uint32 size);
extern uint32 floatToUint(const uint32 floatValue);
extern uint32 uintToFloatRoundDown(const uint32 size);
extern StorageReport storageReport(const Allocator* allocator);
extern StorageReportFull storageReportFull(const Allocator* allocator);

static MunitResult testUintToFloat() {
    // Denorms, exp=1 and exp=2 + mantissa = 0 are all precise.
    // NOTE: Assuming 8 value (3 bit) mantissa.
    // If this test fails, please change this assumption!
    uint32 preciseNumberCount = 17;
    for (uint32 i = 0; i < preciseNumberCount; i++) {
        uint32 roundUp = uintToFloatRoundUp(i);
        uint32 roundDown = uintToFloatRoundDown(i);
        munit_assert_uint(i, ==, roundUp);
        munit_assert_uint(i, ==, roundDown);
    }

    // Test some random picked numbers
    typedef struct {
        uint32 number;
        uint32 up;
        uint32 down;
    } NumberFloatUpDown;

    NumberFloatUpDown testData[] = {
        {.number = 17, .up = 17, .down = 16},
        {.number = 118, .up = 39, .down = 38},
        {.number = 1024, .up = 64, .down = 64},
        {.number = 65536, .up = 112, .down = 112},
        {.number = 529445, .up = 137, .down = 136},
        {.number = 1048575, .up = 144, .down = 143},
    };

    for (uint32 i = 0; i < sizeof(testData) / sizeof(NumberFloatUpDown); i++) {
        NumberFloatUpDown v = testData[i];
        uint32 roundUp = uintToFloatRoundUp(v.number);
        uint32 roundDown = uintToFloatRoundDown(v.number);
        munit_assert_uint(roundUp, ==, v.up);
        munit_assert_uint(roundDown, ==, v.down);
    }
    return MUNIT_OK;
}

static MunitResult testFloatToUint() {
    // Denorms, exp=1 and exp=2 + mantissa = 0 are all precise.
    // NOTE: Assuming 8 value (3 bit) mantissa.
    // If this test fails, please change this assumption!
    uint32 preciseNumberCount = 17;
    for (uint32 i = 0; i < preciseNumberCount; i++) {
        uint32 v = floatToUint(i);
        munit_assert_uint(i, ==, v);
    }

    // Test that float->uint->float conversion is precise for all numbers
    // NOTE: Test values < 240. 240->4G = overflows 32 bit integer
    for (uint32 i = 0; i < 240; i++) {
        uint32 v = floatToUint(i);
        uint32 roundUp = uintToFloatRoundUp(v);
        uint32 roundDown = uintToFloatRoundDown(v);
        munit_assert_uint(i, ==, roundUp);
        munit_assert_uint(i, ==, roundDown);
        // if ((i%8) == 0) printf("\n");
        // printf("%u->%u ", i, v);
    }

    return MUNIT_OK;
}

static MunitResult basicOffsetAllocator() {
    Allocator allocator;
    initAllocator(&allocator, 1024 * 1024 * 256, 128 * 1024);

    Allocation a = allocate(&allocator, 1337);
    uint32 offset = a.offset;
    munit_assert_uint(offset, ==, 0);

    freeAllocation(&allocator, a);
    terminateAllocator(&allocator);
    return MUNIT_OK;
}

static MunitResult testSimpleAllocateOffsetAllocator() {
    Allocator allocator;
    initAllocator(&allocator, 1024 * 1024 * 256, 128 * 1024);

    Allocation a = allocate(&allocator, 0);
    munit_assert_uint(a.offset, ==, 0);

    Allocation b = allocate(&allocator, 1);
    munit_assert_uint(b.offset, ==, 0);

    Allocation c = allocate(&allocator, 123);
    munit_assert_uint(c.offset, ==, 1);

    Allocation d = allocate(&allocator, 1234);
    munit_assert_uint(d.offset, ==, 124);

    freeAllocation(&allocator, a);
    freeAllocation(&allocator, b);
    freeAllocation(&allocator, c);
    freeAllocation(&allocator, d);

    Allocation validateAll = allocate(&allocator, 1024 * 1024 * 256);
    munit_assert_uint(validateAll.offset, ==, 0);

    freeAllocation(&allocator, validateAll);

    terminateAllocator(&allocator);

    return MUNIT_OK;
}

static MunitResult testMergeTrivialOffsetAllocator() {
    Allocator allocator;
    initAllocator(&allocator, 1024 * 1024 * 256, 128 * 1024);

    Allocation a = allocate(&allocator, 1337);
    // munit_assert_uint(a.offset, ==, 0);
    // freeAllocation(&allocator, a);

    Allocation b = allocate(&allocator, 1337);
    // munit_assert_uint(b.offset, ==, 0);

    Allocation c = allocate(&allocator, 13);
    // freeAllocation(&allocator, b);

    Allocation validateAll = allocate(&allocator, 1122);
    // munit_assert_uint(validateAll.offset, ==, 0);

    freeAllocation(&allocator, validateAll);
    terminateAllocator(&allocator);

    return MUNIT_OK;
}

static MunitResult testReuseMergeTrivialOffsetAllocator() {
    Allocator allocator;
    initAllocator(&allocator, 1024 * 1024 * 256, 128 * 1024);

    Allocation a = allocate(&allocator, 1024);
    munit_assert_uint(a.offset, ==, 0);

    Allocation b = allocate(&allocator, 3456);
    munit_assert_uint(b.offset, ==, 1024);

    freeAllocation(&allocator, a);

    Allocation c = allocate(&allocator, 1024);
    munit_assert_uint(c.offset, ==, 0);

    freeAllocation(&allocator, c);
    freeAllocation(&allocator, b);
    Allocation validateAll = allocate(&allocator, 1024 * 1024 * 256);
    munit_assert_uint(validateAll.offset, ==, 0);

    freeAllocation(&allocator, validateAll);
    terminateAllocator(&allocator);

    return MUNIT_OK;
}

static MunitResult testZeroFragmentationOffsetAllocator() {
    Allocator allocator;
    initAllocator(&allocator, 1024 * 1024 * 256, 128 * 1024);

    Allocation allocations[256];

    for (uint32 i = 0; i < 256; i++) {
        allocations[i] = allocate(&allocator, 1024 * 1024);
        munit_assert_uint(allocations[i].offset, ==, i * 1024 * 1024);
    }

    StorageReport report = storageReport(&allocator);
    munit_assert_uint(report.totalFreeSpace, ==, 0);
    munit_assert_uint(report.largestFreeRegion, ==, 0);

    // Free four random slots
    freeAllocation(&allocator, allocations[243]);
    freeAllocation(&allocator, allocations[5]);
    freeAllocation(&allocator, allocations[123]);
    freeAllocation(&allocator, allocations[95]);

    // Free four contiguous slot (allocator must merge)
    freeAllocation(&allocator, allocations[151]);
    freeAllocation(&allocator, allocations[152]);
    freeAllocation(&allocator, allocations[153]);
    freeAllocation(&allocator, allocations[154]);

    allocations[243] = allocate(&allocator, 1024 * 1024);
    allocations[5] = allocate(&allocator, 1024 * 1024);
    allocations[123] = allocate(&allocator, 1024 * 1024);
    allocations[95] = allocate(&allocator, 1024 * 1024);
    allocations[151] = allocate(&allocator, 1024 * 1024 * 4);  // 4x larger
    munit_assert_uint(allocations[243].offset, !=, NO_SPACE);
    munit_assert_uint(allocations[5].offset, !=, NO_SPACE);
    munit_assert_uint(allocations[123].offset, !=, NO_SPACE);
    munit_assert_uint(allocations[95].offset, !=, NO_SPACE);
    munit_assert_uint(allocations[151].offset, !=, NO_SPACE);

    for (uint32 i = 0; i < 256; i++) {
        if (i < 152 || i > 154) {
            freeAllocation(&allocator, allocations[i]);
        }
    }

    StorageReport report2 = storageReport(&allocator);
    munit_assert_uint(report2.totalFreeSpace, ==, 1024 * 1024 * 256);
    munit_assert_uint(report2.largestFreeRegion, ==, 1024 * 1024 * 256);

    Allocation validateAll = allocate(&allocator, 1024 * 1024 * 256);
    munit_assert_uint(validateAll.offset, ==, 0);

    freeAllocation(&allocator, validateAll);
    terminateAllocator(&allocator);

    return MUNIT_OK;
}

static MunitResult testOffsetReuseComplexAllocator() {
    Allocator allocator;
    initAllocator(&allocator, 1024 * 1024 * 256, 128 * 1024);

    Allocation a = allocate(&allocator, 1024);
    munit_assert_uint(a.offset, ==, 0);

    Allocation b = allocate(&allocator, 3456);
    munit_assert_uint(b.offset, ==, 1024);

    freeAllocation(&allocator, a);

    Allocation c = allocate(&allocator, 2345);
    munit_assert_uint(c.offset, ==, 1024 + 3456);

    Allocation d = allocate(&allocator, 456);
    munit_assert_uint(d.offset, ==, 0);

    Allocation e = allocate(&allocator, 512);
    munit_assert_uint(e.offset, ==, 456);

    StorageReport report2 = storageReport(&allocator);
    munit_assert_uint(report2.totalFreeSpace, ==,
                      1024 * 1024 * 256 - 3456 - 2345 - 456 - 512);
    munit_assert_uint(report2.largestFreeRegion, !=, report2.totalFreeSpace);

    freeAllocation(&allocator, c);
    freeAllocation(&allocator, d);
    freeAllocation(&allocator, b);
    freeAllocation(&allocator, e);

    Allocation validateAll = allocate(&allocator, 1024 * 1024 * 256);
    munit_assert_uint(validateAll.offset, ==, 0);

    freeAllocation(&allocator, validateAll);
    terminateAllocator(&allocator);

    return MUNIT_OK;
}

static MunitTest test_suite_tests[] = {
    {"/test_uint_to_float", testUintToFloat, NULL, NULL, MUNIT_TEST_OPTION_NONE,
     NULL},
    {"/test_float_to_uint", testFloatToUint, NULL, NULL, MUNIT_TEST_OPTION_NONE,
     NULL},
    {"/basic_offset_allocator", basicOffsetAllocator, NULL, NULL,
     MUNIT_TEST_OPTION_NONE, NULL},
    {"/test_simple_allocate_offset_allocator",
     testSimpleAllocateOffsetAllocator, NULL, NULL, MUNIT_TEST_OPTION_NONE,
     NULL},
    {"/test_merge_trivial_offset_allocator", testMergeTrivialOffsetAllocator,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/test_reuse_merge_trivial_offset_allocator",
     testReuseMergeTrivialOffsetAllocator, NULL, NULL, MUNIT_TEST_OPTION_NONE,
     NULL},
    {"/test_zero_fragmentation_offset_allocator",
     testZeroFragmentationOffsetAllocator, NULL, NULL, MUNIT_TEST_OPTION_NONE,
     NULL},
    {"/test_offset_reuse_complex_allocator", testOffsetReuseComplexAllocator,
     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    /* Marca el final del array */
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}};

static const MunitSuite suite = {
    "/offset_allocator_tests", /* name */
    test_suite_tests,          /* tests */
    NULL,                      /* suites */
    1,                         /* iterations */
    MUNIT_SUITE_OPTION_NONE    /* options */
};

int main(int argc, char* argv[]) {
    return munit_suite_main(&suite, NULL, argc, argv);
}
