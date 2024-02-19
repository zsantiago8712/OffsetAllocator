// (C) Sebastian Aaltonen 2023
// MIT License (see file: LICENSE)

// #define USE_16_BIT_OFFSETS

typedef unsigned char uint8;
typedef unsigned short uint16;
typedef unsigned int uint32;

// 16 bit offsets mode will halve the metadata storage cost
// But it only supports up to 65536 maximum allocation count
#ifdef USE_16_BIT_NODE_INDICES
typedef uint16 NodeIndex;
#else
typedef uint32 NodeIndex;
#endif

typedef struct _Node* Node;

#define NUM_TOP_BINS 32
#define BINS_PER_LEAF 8
#define TOP_BINS_INDEX_SHIFT 3
#define LEAF_BINS_INDEX_MASK 0x7
#define NUM_LEAF_BINS (NUM_TOP_BINS * BINS_PER_LEAF)

extern const uint32 NO_SPACE;  // Declaración, sin definir aquí

#define EmptyAllocation \
    { .offset = NO_SPACE, .metadata = NO_SPACE }

typedef struct {
    uint32 offset;
    NodeIndex metadata;  // internal: node index
} Allocation;

typedef struct {
    uint32 totalFreeSpace;
    uint32 largestFreeRegion;
} StorageReport;

typedef struct {
    struct {
        uint32 size;
        uint32 count;
    } freeRegions[NUM_LEAF_BINS];
} StorageReportFull;

typedef struct {
    uint32 m_size;
    uint32 m_maxAllocs;
    uint32 m_freeStorage;

    uint32 m_usedBinsTop;
    uint8 m_usedBins[NUM_TOP_BINS];
    NodeIndex m_binIndices[NUM_LEAF_BINS];

    Node m_nodes;
    NodeIndex* m_freeNodes;
    uint32 m_freeOffset;
} Allocator;

void initAllocator(Allocator* allocator,
                   const uint32 size,
                   const uint32 max_allocs);

void resetAllocator(Allocator* allocator);

void terminateAllocator(Allocator* allocator);

Allocation allocate(Allocator* allocator, const uint32 size);

void freeAllocation(Allocator* allocator, Allocation allocation);
