// (C) Sebastian Aaltonen 2023
// MIT License (see file: LICENSE)

#include "offsetAllocator.h"
#include <stdbool.h>
#include <stdlib.h>

#ifdef DEBUG
#include <assert.h>
#define ASSERT(x) assert(x)
// #define DEBUG_VERBOSE
#else
#define ASSERT(x)
#endif

#ifdef DEBUG_VERBOSE
#include <stdio.h>
#endif

#ifdef _MSC_VER
#include <intrin.h>
#endif

#include <string.h>

static inline uint32 lzcnt_nonzero(uint32 v) {
#ifdef _MSC_VER
    unsigned long retVal;
    _BitScanReverse(&retVal, v);
    return 31 - retVal;
#else
    return __builtin_clz(v);
#endif
}

static inline uint32 tzcnt_nonzero(uint32 v) {
#ifdef _MSC_VER
    unsigned long retVal;
    _BitScanForward(&retVal, v);
    return retVal;
#else
    return __builtin_ctz(v);
#endif
}

static const uint32 MANTISSA_BITS = 3;
static const uint32 MANTISSA_VALUE = 1 << MANTISSA_BITS;
static const uint32 MANTISSA_MASK = MANTISSA_VALUE - 1;
static const NodeIndex NODE_UNUSED = 0xffffffff;
const uint32 NO_SPACE = 0xffffffff;

struct _Node {
    uint32 dataOffset;
    uint32 dataSize;
    NodeIndex binListPrev;
    NodeIndex binListNext;
    NodeIndex neighborPrev;
    NodeIndex neighborNext;
    bool used;  // TODO: Merge as bit flag
};

static uint32 insertNodeIntoBin(Allocator* allocator,
                                const uint32 size,
                                const uint32 dataOffset);

static void removeNodeFromBin(Allocator* allocator, const uint32 nodeIndex);

StorageReport storageReport(const Allocator* allocator);
StorageReportFull storageReportFull(const Allocator* allocator);

#define EmptyNode \
    { 0, 0, unused, unused, unused, unused, false }

// Bin sizes follow floating point (exponent + mantissa) distribution (piecewise
// linear log approx) This ensures that for each size class, the average
// overhead percentage stays the same
uint32 uintToFloatRoundUp(const uint32 size) {
    uint32 exp = 0;
    uint32 mantissa = 0;

    if (size < MANTISSA_VALUE) {
        // Denorm: 0..(MANTISSA_VALUE-1)
        mantissa = size;
    } else {
        // Normalized: Hidden high bit always 1. Not stored. Just like float.
        uint32 leadingZeros = lzcnt_nonzero(size);
        uint32 highestSetBit = 31 - leadingZeros;

        uint32 mantissaStartBit = highestSetBit - MANTISSA_BITS;
        exp = mantissaStartBit + 1;
        mantissa = (size >> mantissaStartBit) & MANTISSA_MASK;

        uint32 lowBitsMask = (1 << mantissaStartBit) - 1;

        // Round up!
        if ((size & lowBitsMask) != 0)
            mantissa++;
    }

    return (exp << MANTISSA_BITS) +
           mantissa;  // + allows mantissa->exp overflow for round up
}

uint32 uintToFloatRoundDown(const uint32 size) {
    uint32 exp = 0;
    uint32 mantissa = 0;

    if (size < MANTISSA_VALUE) {
        // Denorm: 0..(MANTISSA_VALUE-1)
        mantissa = size;
    } else {
        // Normalized: Hidden high bit always 1. Not stored. Just like float.
        uint32 leadingZeros = lzcnt_nonzero(size);
        uint32 highestSetBit = 31 - leadingZeros;

        uint32 mantissaStartBit = highestSetBit - MANTISSA_BITS;
        exp = mantissaStartBit + 1;
        mantissa = (size >> mantissaStartBit) & MANTISSA_MASK;
    }

    return (exp << MANTISSA_BITS) | mantissa;
}

uint32 floatToUint(const uint32 floatValue) {
    uint32 exponent = floatValue >> MANTISSA_BITS;
    uint32 mantissa = floatValue & MANTISSA_MASK;
    if (exponent == 0) {
        // Denorms
        return mantissa;
    } else {
        return (mantissa | MANTISSA_VALUE) << (exponent - 1);
    }
}

// Utility functions
uint32 findLowestSetBitAfter(uint32 bitMask, uint32 startBitIndex) {
    uint32 maskBeforeStartIndex = (1 << startBitIndex) - 1;
    uint32 maskAfterStartIndex = ~maskBeforeStartIndex;
    uint32 bitsAfter = bitMask & maskAfterStartIndex;
    if (bitsAfter == 0)
        return NO_SPACE;
    return tzcnt_nonzero(bitsAfter);
}

// Allocator...
void initAllocator(Allocator* allocator,
                   const uint32 size,
                   const uint32 max_allocs) {
    Allocator temp_allocator = {.m_size = size,
                                .m_maxAllocs = max_allocs,
                                .m_nodes = NULL,
                                .m_freeNodes = NULL};

    *allocator = temp_allocator;

    if (sizeof(NodeIndex) == 2) {
        ASSERT(maxAllocs <= 65536);
    }
    resetAllocator(allocator);
}

// Allocator::Allocator(Allocator&& other)
//     : m_size(other.m_size),
//       m_maxAllocs(other.m_maxAllocs),
//       m_freeStorage(other.m_freeStorage),
//       m_usedBinsTop(other.m_usedBinsTop),
//       m_nodes(other.m_nodes),
//       m_freeNodes(other.m_freeNodes),
//       m_freeOffset(other.m_freeOffset) {
//     memcpy(m_usedBins, other.m_usedBins, sizeof(uint8) * NUM_TOP_BINS);
//     memcpy(m_binIndices, other.m_binIndices, sizeof(NodeIndex) *
//     NUM_LEAF_BINS);
//
//     other.m_nodes = nullptr;
//     other.m_freeNodes = nullptr;
//     other.m_freeOffset = 0;
//     other.m_maxAllocs = 0;
//     other.m_usedBinsTop = 0;
// }

void resetAllocator(Allocator* allocator) {
    allocator->m_freeStorage = 0;
    allocator->m_usedBinsTop = 0;
    allocator->m_freeOffset = allocator->m_maxAllocs - 1;

    for (uint32 i = 0; i < NUM_TOP_BINS; i++) {
        allocator->m_usedBins[i] = 0;
    }

    // TODO: cambiar los estatics de estructuras por defines o algo distinto
    for (uint32 i = 0; i < NUM_LEAF_BINS; i++) {
        allocator->m_binIndices[i] = NODE_UNUSED;
    }

    // TODO: cambiar deltes por free
    if (allocator->m_nodes) {
        free(allocator->m_nodes);
    }
    if (allocator->m_freeNodes) {
        free(allocator->m_freeNodes);
    }

    // TODO: cambiar new por malloc o calloc
    allocator->m_nodes =
        (Node)calloc(allocator->m_maxAllocs, sizeof(struct _Node));
    allocator->m_freeNodes =
        (NodeIndex*)calloc(allocator->m_maxAllocs, sizeof(NodeIndex));

    // Freelist is a stack. Nodes in inverse order so that [0] pops first.
    for (uint32 i = 0; i < allocator->m_maxAllocs; i++) {
        allocator->m_freeNodes[i] = allocator->m_maxAllocs - i - 1;
        allocator->m_nodes[i].binListNext = NODE_UNUSED;
        allocator->m_nodes[i].binListPrev = NODE_UNUSED;
        allocator->m_nodes[i].neighborPrev = NODE_UNUSED;
        allocator->m_nodes[i].neighborNext = NODE_UNUSED;
    }

    // Start state: Whole storage as one big node
    // Algorithm will split remainders and push them back as smaller nodes
    // TODO: crear funncion insertNodeIntoBin
    insertNodeIntoBin(allocator, allocator->m_size, 0);
}

void terminateAllocator(Allocator* allocator) {
    free(allocator->m_nodes);
    free(allocator->m_freeNodes);
}

Allocation allocate(Allocator* allocator, const uint32 size) {
    // Out of allocations?
    //
    Allocation res = EmptyAllocation;
    if (allocator->m_freeOffset == 0) {
        return res;
    }

    // Round up to bin index to ensure that alloc >= bin
    // Gives us min bin index that fits the size
    uint32 minBinIndex = uintToFloatRoundUp(size);

    uint32 minTopBinIndex = minBinIndex >> TOP_BINS_INDEX_SHIFT;
    uint32 minLeafBinIndex = minBinIndex & LEAF_BINS_INDEX_MASK;

    uint32 topBinIndex = minTopBinIndex;
    uint32 leafBinIndex = NO_SPACE;

    // If top bin exists, scan its leaf bin. This can fail (NO_SPACE).
    if (allocator->m_usedBinsTop & (1 << topBinIndex)) {
        leafBinIndex = findLowestSetBitAfter(allocator->m_usedBins[topBinIndex],
                                             minLeafBinIndex);
    }

    // If we didn't find space in top bin, we search top bin from +1
    if (leafBinIndex == NO_SPACE) {
        topBinIndex =
            findLowestSetBitAfter(allocator->m_usedBinsTop, minTopBinIndex + 1);

        // Out of space?
        if (topBinIndex == NO_SPACE) {
            return res;
        }

        // All leaf bins here fit the alloc, since the top bin was rounded up.
        // Start leaf search from bit 0. NOTE: This search can't fail since at
        // least one leaf bit was set because the top bit was set.
        leafBinIndex = tzcnt_nonzero(allocator->m_usedBins[topBinIndex]);
    }

    uint32 binIndex = (topBinIndex << TOP_BINS_INDEX_SHIFT) | leafBinIndex;

    // Pop the top node of the bin. Bin top = node.next.
    uint32 nodeIndex = allocator->m_binIndices[binIndex];
    Node node = &(allocator->m_nodes[nodeIndex]);
    uint32 nodeTotalSize = node->dataSize;
    node->dataSize = size;
    node->used = true;
    allocator->m_binIndices[binIndex] = node->binListNext;
    if (node->binListNext != NODE_UNUSED)
        allocator->m_nodes[node->binListNext].binListPrev = NODE_UNUSED;
    allocator->m_freeStorage -= nodeTotalSize;
#ifdef DEBUG_VERBOSE
    printf("Free storage: %u (-%u) (allocate)\n", allocator->m_freeStorage,
           nodeTotalSize);
#endif

    // Bin empty?
    if (allocator->m_binIndices[binIndex] == NODE_UNUSED) {
        // Remove a leaf bin mask bit
        allocator->m_usedBins[topBinIndex] &= ~(1 << leafBinIndex);

        // All leaf bins empty?
        if (allocator->m_usedBins[topBinIndex] == 0) {
            // Remove a top bin mask bit
            allocator->m_usedBinsTop &= ~(1 << topBinIndex);
        }
    }

    // Push back reminder N elements to a lower bin
    uint32 reminderSize = nodeTotalSize - size;
    if (reminderSize > 0) {
        uint32 newNodeIndex =
            insertNodeIntoBin(allocator, reminderSize, node->dataOffset + size);

        // Link nodes next to each other so that we can merge them later if both
        // are free And update the old next neighbor to point to the new node
        // (in middle)
        if (node->neighborNext != NODE_UNUSED) {
            allocator->m_nodes[node->neighborNext].neighborPrev = newNodeIndex;
        }
        allocator->m_nodes[newNodeIndex].neighborPrev = nodeIndex;
        allocator->m_nodes[newNodeIndex].neighborNext = node->neighborNext;
        node->neighborNext = newNodeIndex;
    }

    res.offset = node->dataOffset;
    res.metadata = nodeIndex;

    return res;
}

void freeAllocation(Allocator* allocator, Allocation allocation) {
    ASSERT(allocation.metadata != NO_SPACE);
    if (!allocator->m_nodes)
        return;

    uint32 nodeIndex = allocation.metadata;
    Node node = &(allocator->m_nodes[nodeIndex]);

    // Double delete check
    ASSERT(node.used == true);

    // Merge with neighbors...
    uint32 offset = node->dataOffset;
    uint32 size = node->dataSize;

    if ((node->neighborPrev != NODE_UNUSED) &&
        (allocator->m_nodes[node->neighborPrev].used == false)) {
        // Previous (contiguous) free node: Change offset to previous node
        // offset. Sum sizes
        Node prevNode = &(allocator->m_nodes[node->neighborPrev]);
        offset = prevNode->dataOffset;
        size += prevNode->dataSize;

        // Remove node from the bin linked list and put it in the freelist
        removeNodeFromBin(allocator, node->neighborPrev);

        ASSERT(prevNode.neighborNext == nodeIndex);
        node->neighborPrev = prevNode->neighborPrev;
    }

    if ((node->neighborNext != NODE_UNUSED) &&
        (allocator->m_nodes[node->neighborNext].used == false)) {
        // Next (contiguous) free node: Offset remains the same. Sum sizes.
        Node nextNode = &(allocator->m_nodes[node->neighborNext]);
        size += nextNode->dataSize;

        // Remove node from the bin linked list and put it in the freelist
        removeNodeFromBin(allocator, node->neighborNext);

        ASSERT(nextNode->neighborPrev == nodeIndex);
        node->neighborNext = nextNode->neighborNext;
    }

    uint32 neighborNext = node->neighborNext;
    uint32 neighborPrev = node->neighborPrev;

    // Insert the removed node to freelist
#ifdef DEBUG_VERBOSE
    printf("Putting node %u into freelist[%u] (free)\n", nodeIndex,
           m_freeOffset + 1);
#endif
    allocator->m_freeNodes[++allocator->m_freeOffset] = nodeIndex;

    // Insert the (combined) free node to bin
    uint32 combinedNodeIndex = insertNodeIntoBin(allocator, size, offset);

    // Connect neighbors with the new combined node
    if (neighborNext != NODE_UNUSED) {
        allocator->m_nodes[combinedNodeIndex].neighborNext = neighborNext;
        allocator->m_nodes[neighborNext].neighborPrev = combinedNodeIndex;
    }
    if (neighborPrev != NODE_UNUSED) {
        allocator->m_nodes[combinedNodeIndex].neighborPrev = neighborPrev;
        allocator->m_nodes[neighborPrev].neighborNext = combinedNodeIndex;
    }
}

static uint32 insertNodeIntoBin(Allocator* allocator,
                                const uint32 size,
                                const uint32 dataOffset) {
    // Round down to bin index to ensure that bin >= alloc
    uint32 binIndex = uintToFloatRoundDown(size);

    uint32 topBinIndex = binIndex >> TOP_BINS_INDEX_SHIFT;
    uint32 leafBinIndex = binIndex & LEAF_BINS_INDEX_MASK;

    // Bin was empty before?
    if (allocator->m_binIndices[binIndex] == NODE_UNUSED) {
        // Set bin mask bits
        allocator->m_usedBins[topBinIndex] |= 1 << leafBinIndex;
        allocator->m_usedBinsTop |= 1 << topBinIndex;
    }

    // Take a freelist node and insert on top of the bin linked list (next = old
    // top)
    uint32 topNodeIndex = allocator->m_binIndices[binIndex];
    uint32 nodeIndex = allocator->m_freeNodes[allocator->m_freeOffset--];
#ifdef DEBUG_VERBOSE
    printf("Getting node %u from freelist[%u]\n", nodeIndex,
           allocator->m_freeOffset + 1);
#endif
    allocator->m_nodes[nodeIndex].dataOffset = dataOffset;
    allocator->m_nodes[nodeIndex].dataSize = size;
    allocator->m_nodes[nodeIndex].binListNext = topNodeIndex;
    allocator->m_nodes[nodeIndex].binListPrev = NODE_UNUSED;
    allocator->m_nodes[nodeIndex].neighborPrev = NODE_UNUSED;
    allocator->m_nodes[nodeIndex].neighborNext = NODE_UNUSED;
    allocator->m_nodes[nodeIndex].used = false;

    if (topNodeIndex != NODE_UNUSED) {
        allocator->m_nodes[topNodeIndex].binListPrev = nodeIndex;
    }
    allocator->m_binIndices[binIndex] = nodeIndex;

    allocator->m_freeStorage += size;
#ifdef DEBUG_VERBOSE
    printf("Free storage: %u (+%u) (insertNodeIntoBin)\n",
           allocator->m_freeStorage, size);
#endif

    return nodeIndex;
}

static void removeNodeFromBin(Allocator* allocator, const uint32 nodeIndex) {
    Node node = &(allocator->m_nodes[nodeIndex]);

    if (node->binListPrev != NODE_UNUSED) {
        // Easy case: We have previous node. Just remove this node from the
        // middle of the list.
        allocator->m_nodes[node->binListPrev].binListNext = node->binListNext;
        if (node->binListNext != NODE_UNUSED) {
            allocator->m_nodes[node->binListNext].binListPrev =
                node->binListPrev;
        }
    } else {
        // Hard case: We are the first node in a bin. Find the bin.

        // Round down to bin index to ensure that bin >= alloc
        uint32 binIndex = uintToFloatRoundDown(node->dataSize);

        uint32 topBinIndex = binIndex >> TOP_BINS_INDEX_SHIFT;
        uint32 leafBinIndex = binIndex & LEAF_BINS_INDEX_MASK;

        allocator->m_binIndices[binIndex] = node->binListNext;
        if (node->binListNext != NODE_UNUSED) {
            allocator->m_nodes[node->binListNext].binListPrev = NODE_UNUSED;
        }

        // Bin empty?
        if (allocator->m_binIndices[binIndex] == NODE_UNUSED) {
            // Remove a leaf bin mask bit
            allocator->m_usedBins[topBinIndex] &= ~(1 << leafBinIndex);

            // All leaf bins empty?
            if (allocator->m_usedBins[topBinIndex] == 0) {
                // Remove a top bin mask bit
                allocator->m_usedBinsTop &= ~(1 << topBinIndex);
            }
        }
    }

    // Insert the node to freelist
#ifdef DEBUG_VERBOSE
    printf("Putting node %u into freelist[%u] (removeNodeFromBin)\n", nodeIndex,
           allocator->m_freeOffset + 1);
#endif
    allocator->m_freeNodes[++allocator->m_freeOffset] = nodeIndex;

    allocator->m_freeStorage -= node->dataSize;
#ifdef DEBUG_VERBOSE
    printf("Free storage: %u (-%u) (removeNodeFromBin)\n",
           allocator->m_freeStorage, node->dataSize);
#endif
}

static uint32 allocationSize(const Allocator* allocator,
                             const Allocation allocation) {
    if (allocation.metadata == NO_SPACE)
        return 0;
    if (!allocator->m_nodes)
        return 0;

    return allocator->m_nodes[allocation.metadata].dataSize;
}

StorageReport storageReport(const Allocator* allocator) {
    uint32 largestFreeRegion = 0;
    uint32 freeStorage = 0;

    // Out of allocations? -> Zero free space
    if (allocator->m_freeOffset > 0) {
        freeStorage = allocator->m_freeStorage;
        if (allocator->m_usedBinsTop) {
            uint32 topBinIndex = 31 - lzcnt_nonzero(allocator->m_usedBinsTop);
            uint32 leafBinIndex =
                31 - lzcnt_nonzero(allocator->m_usedBins[topBinIndex]);
            largestFreeRegion = floatToUint(
                (topBinIndex << TOP_BINS_INDEX_SHIFT) | leafBinIndex);
            ASSERT(freeStorage >= largestFreeRegion);
        }
    }
    StorageReport report = {.totalFreeSpace = freeStorage,
                            .largestFreeRegion = largestFreeRegion};
    return report;
}

StorageReportFull storageReportFull(const Allocator* allocator) {
    StorageReportFull report;
    for (uint32 i = 0; i < NUM_LEAF_BINS; i++) {
        uint32 count = 0;
        uint32 nodeIndex = allocator->m_binIndices[i];
        while (nodeIndex != NODE_UNUSED) {
            nodeIndex = allocator->m_nodes[nodeIndex].binListNext;
            count++;
        }
        report.freeRegions[i].size = floatToUint(i);
        report.freeRegions[i].count = count;
    }
    return report;
}
