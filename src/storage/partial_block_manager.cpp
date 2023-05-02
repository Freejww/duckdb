#include "duckdb/storage/partial_block_manager.hpp"

namespace duckdb {

PartialBlockManager::PartialBlockManager(BlockManager &block_manager, CheckpointType checkpoint_type,
                                         uint32_t max_partial_block_size, uint32_t max_use_count)
    : block_manager(block_manager), checkpoint_type(checkpoint_type), max_partial_block_size(max_partial_block_size),
      max_use_count(max_use_count) {
}
PartialBlockManager::~PartialBlockManager() {
}
//===--------------------------------------------------------------------===//
// Partial Blocks
//===--------------------------------------------------------------------===//
PartialBlockAllocation PartialBlockManager::GetBlockAllocation(uint32_t segment_size) {
	PartialBlockAllocation allocation;
	allocation.block_manager = &block_manager;
	allocation.allocation_size = segment_size;

	// if the block is less than 80% full, we consider it a "partial block"
	// which means we will try to fit it with other blocks
	// check if there is a partial block available we can write to
	if (segment_size <= max_partial_block_size && GetPartialBlock(segment_size, allocation.partial_block)) {
		//! there is! increase the reference count of this block
		allocation.partial_block->state.block_use_count += 1;
		allocation.state = allocation.partial_block->state;
		if (checkpoint_type == CheckpointType::FULL_CHECKPOINT) {
			block_manager.IncreaseBlockReferenceCount(allocation.state.block_id);
		}
	} else {
		// full block: get a free block to write to
		AllocateBlock(allocation.state, segment_size);
	}
	return allocation;
}

bool PartialBlockManager::HasBlockAllocation(uint32_t segment_size) {
	return segment_size <= max_partial_block_size &&
	       partially_filled_blocks.lower_bound(segment_size) != partially_filled_blocks.end();
}

void PartialBlockManager::AllocateBlock(PartialBlockState &state, uint32_t segment_size) {
	D_ASSERT(segment_size <= Storage::BLOCK_SIZE);
	if (checkpoint_type == CheckpointType::FULL_CHECKPOINT) {
		state.block_id = block_manager.GetFreeBlockId();
	} else {
		state.block_id = INVALID_BLOCK;
	}
	state.block_size = Storage::BLOCK_SIZE;
	state.offset_in_block = 0;
	state.block_use_count = 1;
}

bool PartialBlockManager::GetPartialBlock(idx_t segment_size, unique_ptr<PartialBlock> &partial_block) {
	auto entry = partially_filled_blocks.lower_bound(segment_size);
	if (entry == partially_filled_blocks.end()) {
		return false;
	}
	// found a partially filled block! fill in the info
	partial_block = std::move(entry->second);
	partially_filled_blocks.erase(entry);

	D_ASSERT(partial_block->state.offset_in_block > 0);
	D_ASSERT(ValueIsAligned(partial_block->state.offset_in_block));
	return true;
}

void PartialBlockManager::RegisterPartialBlock(PartialBlockAllocation &&allocation) {
	auto &state = allocation.partial_block->state;
	if (state.block_use_count < max_use_count) {
		auto unaligned_size = allocation.allocation_size + state.offset_in_block;
		auto new_size = AlignValue(unaligned_size);
		if (new_size != unaligned_size) {
			// register the uninitialized region so we can correctly initialize it before writing to disk
			allocation.partial_block->AddUninitializedRegion(unaligned_size, new_size);
		}
		state.offset_in_block = new_size;
		auto new_space_left = state.block_size - new_size;
		// check if the block is STILL partially filled after adding the segment_size
		if (new_space_left >= Storage::BLOCK_SIZE - max_partial_block_size) {
			// the block is still partially filled: add it to the partially_filled_blocks list
			partially_filled_blocks.insert(make_pair(new_space_left, std::move(allocation.partial_block)));
		}
	}
	idx_t free_space = state.block_size - state.offset_in_block;
	auto block_to_free = std::move(allocation.partial_block);
	if (!block_to_free && partially_filled_blocks.size() > MAX_BLOCK_MAP_SIZE) {
		// Free the page with the least space free.
		auto itr = partially_filled_blocks.begin();
		block_to_free = std::move(itr->second);
		free_space = state.block_size - itr->first;
		partially_filled_blocks.erase(itr);
	}
	// Flush any block that we're not going to reuse.
	if (block_to_free) {
		block_to_free->Flush(free_space);
		AddWrittenBlock(block_to_free->state.block_id);
	}
}

void PartialBlock::Merge(PartialBlock &other, idx_t offset, idx_t other_size) {
	throw InternalException("PartialBlock::Merge not implemented for this block type");
}

void PartialBlockManager::Merge(PartialBlockManager &other) {
	if (&other == this) {
		throw InternalException("Cannot merge into itself");
	}
	// for each partially filled block in the other manager, check if we can merge it into an existing block in this
	// manager
	for (auto &e : other.partially_filled_blocks) {
		if (!e.second) {
			throw InternalException("Empty partially filled block found");
		}
		auto used_space = Storage::BLOCK_SIZE - e.first;
		if (HasBlockAllocation(used_space)) {
			// we can merge this block into an existing block - merge them
			// merge blocks
			auto allocation = GetBlockAllocation(used_space);
			allocation.partial_block->Merge(*e.second, allocation.state.offset_in_block, used_space);

			// re-register the partial block
			allocation.state.offset_in_block += used_space;
			RegisterPartialBlock(std::move(allocation));
		} else {
			// we cannot merge this block - append it directly to the current block manager
			partially_filled_blocks.insert(make_pair(e.first, std::move(e.second)));
		}
	}
	// copy over the written blocks
	for (auto &block_id : other.written_blocks) {
		AddWrittenBlock(block_id);
	}
	other.written_blocks.clear();
	other.partially_filled_blocks.clear();
}

void PartialBlockManager::AddWrittenBlock(block_id_t block) {
	auto entry = written_blocks.insert(block);
	if (!entry.second) {
		throw InternalException("Written block already exists");
	}
}

void PartialBlockManager::FlushPartialBlocks() {
	for (auto &e : partially_filled_blocks) {
		e.second->Flush(e.first);
	}
	partially_filled_blocks.clear();
}

void PartialBlockManager::Rollback() {
	partially_filled_blocks.clear();
	for (auto &block_id : written_blocks) {
		block_manager.MarkBlockAsFree(block_id);
	}
}

} // namespace duckdb
