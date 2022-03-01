//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// extendible_hash_table.cpp
//
// Identification: src/container/hash/extendible_hash_table.cpp
//
// Copyright (c) 2015-2021, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "common/exception.h"
#include "common/logger.h"
#include "common/rid.h"
#include "container/hash/extendible_hash_table.h"

namespace bustub {

template <typename KeyType, typename ValueType, typename KeyComparator>
HASH_TABLE_TYPE::ExtendibleHashTable(const std::string &name, BufferPoolManager *buffer_pool_manager,
                                     const KeyComparator &comparator, HashFunction<KeyType> hash_fn)
    : buffer_pool_manager_(buffer_pool_manager), comparator_(comparator), hash_fn_(std::move(hash_fn)) {
  // 由于还没有bucket，故要初始化directory_page_id_为无效值
  directory_page_id_ = INVALID_PAGE_ID;
}

/*****************************************************************************
 * HELPERS
 *****************************************************************************/
/**
 * Hash - simple helper to downcast MurmurHash's 64-bit hash to 32-bit
 * for extendible hashing.
 *
 * @param key the key to hash
 * @return the downcasted 32-bit hash
 */
template <typename KeyType, typename ValueType, typename KeyComparator>
uint32_t HASH_TABLE_TYPE::Hash(KeyType key) {
  return static_cast<uint32_t>(hash_fn_.GetHash(key));
}

template <typename KeyType, typename ValueType, typename KeyComparator>
inline uint32_t HASH_TABLE_TYPE::KeyToDirectoryIndex(KeyType key, HashTableDirectoryPage *dir_page) {
  // 直接用掩码做与运算即可 将key哈希后转换为目录索引
  return Hash(key) & dir_page->GetGlobalDepthMask();
}

template <typename KeyType, typename ValueType, typename KeyComparator>
inline uint32_t HASH_TABLE_TYPE::KeyToPageId(KeyType key, HashTableDirectoryPage *dir_page) {
  // 得到该key在哪个桶中
  return dir_page->GetBucketPageId(KeyToDirectoryIndex(key, dir_page));
}

/**
 * 从BufferPoolManager中得到directory所在的Page，Page中是一个Directory对象
 * 如果还没有directory的话，那就创建一个
 *
 * @return a pointer to the directory page
 */
template <typename KeyType, typename ValueType, typename KeyComparator>
HashTableDirectoryPage *HASH_TABLE_TYPE::FetchDirectoryPage() {
  HashTableDirectoryPage *ret;
  // 目录结构可能发生修改,因此目录上锁
  directory_lock_.lock();
  // 如果不可用，则创建一个
  if (directory_page_id_ == INVALID_PAGE_ID) {
    // 先创建DirectoryPage
    page_id_t new_page_id_dir;
    Page *page = buffer_pool_manager_->NewPage(&new_page_id_dir);
    assert(page != nullptr);
    ret = reinterpret_cast<HashTableDirectoryPage *>(page->GetData());
    directory_page_id_ = new_page_id_dir;
    ret->SetPageId(directory_page_id_);
    // 然后为空的directory创建第一个bucket
    page_id_t new_page_id_buc;
    page = nullptr;
    page = buffer_pool_manager_->NewPage(&new_page_id_buc);
    assert(page != nullptr);
    ret->SetBucketPageId(0, new_page_id_buc);
    // 最后Unpin这两个页面
    assert(buffer_pool_manager_->UnpinPage(new_page_id_dir, true));
    assert(buffer_pool_manager_->UnpinPage(new_page_id_buc, true));
  }
  directory_lock_.unlock();

  // 从buffer中获取页面
  assert(directory_page_id_ != INVALID_PAGE_ID);
  Page *page = buffer_pool_manager_->FetchPage(directory_page_id_);
  assert(page != nullptr);
  ret = reinterpret_cast<HashTableDirectoryPage *>(page->GetData());
  return ret;
}

/**
 * 使用pageid从BufferPoolManager中得到一个Page，其GetData就是bucket对象。
 * 添加一个函数GetBucketPageData，将Page与data分开处理
 *
 * @param bucket_page_id the page_id to fetch
 * @return a pointer to a bucket page
 */
template <typename KeyType, typename ValueType, typename KeyComparator>
Page *HASH_TABLE_TYPE::FetchBucketPage(page_id_t bucket_page_id) {
  Page *page = buffer_pool_manager_->FetchPage(bucket_page_id);
  assert(page != nullptr);
  return page;
}

// 将page转换为bucket page
template <typename KeyType, typename ValueType, typename KeyComparator>
HASH_TABLE_BUCKET_TYPE *HASH_TABLE_TYPE::GetBucketPageData(Page *page) {
  return reinterpret_cast<HASH_TABLE_BUCKET_TYPE *>(page->GetData());
}

/*****************************************************************************
 * SEARCH
 *****************************************************************************/
/**
 * 在hash table上按key查找该key对应哪些value
 *
 * @param transaction the current transaction
 * @param key the key to look up
 * @param[out] result the value(s) associated with a given key
 * @return the value(s) associated with the given key
 */
template <typename KeyType, typename ValueType, typename KeyComparator>
bool HASH_TABLE_TYPE::GetValue(Transaction *transaction, const KeyType &key, std::vector<ValueType> *result) {
  table_latch_.RLock();  // 目录读锁
  // 得到哈希目录 得到key所在的桶 拉取桶中信息
  HashTableDirectoryPage *dir_page = FetchDirectoryPage();
  page_id_t bucket_page_id = KeyToPageId(key, dir_page);
  Page *bucket_page = FetchBucketPage(bucket_page_id);
  bucket_page->RLatch();  // 桶页的读锁
  HASH_TABLE_BUCKET_TYPE *bucket = GetBucketPageData(bucket_page);

  // 读取数据 即得到该key对应的value有哪些
  bool ret = bucket->GetValue(key, comparator_, result);
  bucket_page->RUnlatch();

  // 记得Fetch后要Unpin
  assert(buffer_pool_manager_->UnpinPage(bucket_page_id, false));
  assert(buffer_pool_manager_->UnpinPage(dir_page->GetPageId(), false));

  table_latch_.RUnlock();
  return ret;
}

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
// 向哈希表(的桶)中插入键值对
template <typename KeyType, typename ValueType, typename KeyComparator>
bool HASH_TABLE_TYPE::Insert(Transaction *transaction, const KeyType &key, const ValueType &value) {
  table_latch_.RLock();
  HashTableDirectoryPage *dir_page = FetchDirectoryPage();
  page_id_t bucket_page_id = KeyToPageId(key, dir_page);
  Page *bucket_page = FetchBucketPage(bucket_page_id);
  bucket_page->WLatch();
  HASH_TABLE_BUCKET_TYPE *bucket = GetBucketPageData(bucket_page);

  // 如果bucket没满，直接插入即可
  if (!bucket->IsFull()) {
    bool ret = bucket->Insert(key, value, comparator_);
    bucket_page->WUnlatch();
    assert(buffer_pool_manager_->UnpinPage(bucket_page_id, true));
    assert(buffer_pool_manager_->UnpinPage(dir_page->GetPageId(), false));
    table_latch_.RUnlock();
    return ret;
  }
  // 满了要扩容
  bucket_page->WUnlatch();
  assert(buffer_pool_manager_->UnpinPage(bucket_page_id, false));
  assert(buffer_pool_manager_->UnpinPage(dir_page->GetPageId(), false));
  table_latch_.RUnlock();
  return SplitInsert(transaction, key, value);
}

// 将桶分裂并插入
template <typename KeyType, typename ValueType, typename KeyComparator>
bool HASH_TABLE_TYPE::SplitInsert(Transaction *transaction, const KeyType &key, const ValueType &value) {
  table_latch_.WLock();
  // 得到哈希目录 得到key对应的目录项以及该目录项对应桶的局部深度
  HashTableDirectoryPage *dir_page = FetchDirectoryPage();
  int64_t split_bucket_index = KeyToDirectoryIndex(key, dir_page);
  uint32_t split_bucket_depth = dir_page->GetLocalDepth(split_bucket_index);

  // 容量满了，不能扩了
  if (split_bucket_depth >= MAX_BUCKET_DEPTH) {
    assert(buffer_pool_manager_->UnpinPage(dir_page->GetPageId(), false));
    table_latch_.WUnlock();
    return false;
  }

  // 看看Directory需不需要扩容
  if (split_bucket_depth == dir_page->GetGlobalDepth()) {
    dir_page->IncrGlobalDepth();
  }

  // 增加local depth
  dir_page->IncrLocalDepth(split_bucket_index);

  // 获取当前bucket，先将数据保存下来，然后重新初始化它
  page_id_t split_bucket_page_id = KeyToPageId(key, dir_page);
  Page *split_bucket_page = FetchBucketPage(split_bucket_page_id);
  split_bucket_page->WLatch();
  HASH_TABLE_BUCKET_TYPE *split_bucket = GetBucketPageData(split_bucket_page);
  uint32_t origin_array_size = split_bucket->NumReadable();
  MappingType *origin_array = split_bucket->GetArrayCopy();
  split_bucket->Reset();

  // 创建一个image bucket，并初始化该image bucket
  page_id_t image_bucket_page_id;  // image bucket所在的页
  Page *image_bucket_page = buffer_pool_manager_->NewPage(&image_bucket_page_id);
  assert(image_bucket_page != nullptr);
  image_bucket_page->WLatch();
  HASH_TABLE_BUCKET_TYPE *image_bucket = GetBucketPageData(image_bucket_page);
  // image bucket的目录索引
  uint32_t split_image_bucket_index = dir_page->GetSplitImageIndex(split_bucket_index);
  // 在目录中修改目录项：设置image bucket的目录索引以及image bucket的局部深度
  dir_page->SetLocalDepth(split_image_bucket_index, dir_page->GetLocalDepth(split_bucket_index));
  dir_page->SetBucketPageId(split_image_bucket_index, image_bucket_page_id);

  // 将所有同一级的bucket设置为相同的local depth和page
  uint32_t diff = 1 << dir_page->GetLocalDepth(split_bucket_index);
  for (uint32_t i = split_bucket_index; i >= diff; i -= diff) {
    dir_page->SetBucketPageId(i, split_bucket_page_id);
    dir_page->SetLocalDepth(i, dir_page->GetLocalDepth(split_bucket_index));
  }
  for (uint32_t i = split_bucket_index; i < dir_page->Size(); i += diff) {
    dir_page->SetBucketPageId(i, split_bucket_page_id);
    dir_page->SetLocalDepth(i, dir_page->GetLocalDepth(split_bucket_index));
  }
  for (uint32_t i = split_image_bucket_index; i >= diff; i -= diff) {
    dir_page->SetBucketPageId(i, image_bucket_page_id);
    dir_page->SetLocalDepth(i, dir_page->GetLocalDepth(split_bucket_index));
  }
  for (uint32_t i = split_image_bucket_index; i < dir_page->Size(); i += diff) {
    dir_page->SetBucketPageId(i, image_bucket_page_id);
    dir_page->SetLocalDepth(i, dir_page->GetLocalDepth(split_bucket_index));
  }

  // 重新插入数据
  for (uint32_t i = 0; i < origin_array_size; i++) {
    MappingType tmp = origin_array[i];
    // 得到目录索引
    uint32_t target_bucket_index = Hash(tmp.first) & dir_page->GetLocalDepthMask(split_bucket_index);
    // 该目录索引对应的桶
    page_id_t target_bucket_index_page = dir_page->GetBucketPageId(target_bucket_index);
    assert(target_bucket_index_page == split_bucket_page_id || target_bucket_index_page == image_bucket_page_id);
    // 这里根据新计算的hash结果决定插入哪个bucket
    if (target_bucket_index_page == split_bucket_page_id) {
      assert(split_bucket->Insert(tmp.first, tmp.second, comparator_));
    } else {
      assert(image_bucket->Insert(tmp.first, tmp.second, comparator_));
    }
  }
  delete[] origin_array;

  split_bucket_page->WUnlatch();
  image_bucket_page->WUnlatch();

  // Unpin
  assert(buffer_pool_manager_->UnpinPage(split_bucket_page_id, true));
  assert(buffer_pool_manager_->UnpinPage(image_bucket_page_id, true));
  assert(buffer_pool_manager_->UnpinPage(dir_page->GetPageId(), true));

  // 最后重新尝试插入
  return Insert(transaction, key, value);
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/
// 给定key 删除该key对应的键值对
template <typename KeyType, typename ValueType, typename KeyComparator>
bool HASH_TABLE_TYPE::Remove(Transaction *transaction, const KeyType &key, const ValueType &value) {
  table_latch_.RLock();
  // 先找到bucket
  HashTableDirectoryPage *dir_page = FetchDirectoryPage();
  page_id_t bucket_page_id = KeyToPageId(key, dir_page);
  uint32_t bucket_index = KeyToDirectoryIndex(key, dir_page);
  Page *bucket_page = FetchBucketPage(bucket_page_id);
  bucket_page->WLatch();
  HASH_TABLE_BUCKET_TYPE *bucket = GetBucketPageData(bucket_page);

  // 删除Key-value
  bool ret = bucket->Remove(key, value, comparator_);

  // 若删除后桶为空则合并
  if (bucket->IsEmpty()) {
    bucket_page->WUnlatch();
    // Unpin
    assert(buffer_pool_manager_->UnpinPage(bucket_page_id, true));
    assert(buffer_pool_manager_->UnpinPage(dir_page->GetPageId(), false));
    table_latch_.RUnlock();
    Merge(transaction, bucket_index);
    return ret;
  }
  bucket_page->WUnlatch();
  // Unpin
  assert(buffer_pool_manager_->UnpinPage(bucket_page_id, true));
  assert(buffer_pool_manager_->UnpinPage(dir_page->GetPageId(), false));
  table_latch_.RUnlock();
  return ret;
}

/*****************************************************************************
 * MERGE
 *****************************************************************************/
/**
 * target_bucket_index是要被删除的bucket的index
 * 将一个空桶和他的兄弟桶合并。若Remove函数使得一个桶为空,则Remove调用该函数
 *
 * 以下三种情况将跳过merge操作:
 * 1. 桶不再为空(可能由其他线程修改).
 * 2. 桶的局部深度为0.
 * 3. 若桶的local depth不等于它split image的local depth.
 */
template <typename KeyType, typename ValueType, typename KeyComparator>
void HASH_TABLE_TYPE::Merge(Transaction *transaction, uint32_t target_bucket_index) {
  table_latch_.WLock();
  // 得到目录页 根据索引得到桶页 得到image bucket的索引
  HashTableDirectoryPage *dir_page = FetchDirectoryPage();
  page_id_t target_bucket_page_id = dir_page->GetBucketPageId(target_bucket_index);
  uint32_t image_bucket_index = dir_page->GetSplitImageIndex(target_bucket_index);

  // local depth为0说明已经最小了，不收缩
  uint32_t local_depth = dir_page->GetLocalDepth(target_bucket_index);
  if (local_depth == 0) {
    assert(buffer_pool_manager_->UnpinPage(dir_page->GetPageId(), false));
    table_latch_.WUnlock();
    return;
  }

  // 如果该bucket与其split image深度不同，也不收缩
  if (local_depth != dir_page->GetLocalDepth(image_bucket_index)) {
    assert(buffer_pool_manager_->UnpinPage(dir_page->GetPageId(), false));
    table_latch_.WUnlock();
    return;
  }

  // 如果target bucket不为空，则不收缩
  Page *target_bucket_page = FetchBucketPage(target_bucket_page_id);
  target_bucket_page->RLatch();
  HASH_TABLE_BUCKET_TYPE *target_bucket = GetBucketPageData(target_bucket_page);
  if (!target_bucket->IsEmpty()) {
    target_bucket_page->RUnlatch();
    assert(buffer_pool_manager_->UnpinPage(target_bucket_page_id, false));
    assert(buffer_pool_manager_->UnpinPage(dir_page->GetPageId(), false));
    table_latch_.WUnlock();
    return;
  }

  target_bucket_page->RUnlatch();
  // 删除target bucket，此时该bucket已经为空
  assert(buffer_pool_manager_->UnpinPage(target_bucket_page_id, false));
  assert(buffer_pool_manager_->DeletePage(target_bucket_page_id));

  // 设置target bucket的page为split image的page，即合并target和split
  page_id_t image_bucket_page_id = dir_page->GetBucketPageId(image_bucket_index);
  dir_page->SetBucketPageId(target_bucket_index, image_bucket_page_id);
  dir_page->DecrLocalDepth(target_bucket_index);
  dir_page->DecrLocalDepth(image_bucket_index);
  assert(dir_page->GetLocalDepth(target_bucket_index) == dir_page->GetLocalDepth(image_bucket_index));

  // 遍历整个directory，将所有指向target bucket page的bucket全部重新指向split image bucket的page
  // 局部深度减1
  for (uint32_t i = 0; i < dir_page->Size(); i++) {
    if (dir_page->GetBucketPageId(i) == target_bucket_page_id || dir_page->GetBucketPageId(i) == image_bucket_page_id) {
      dir_page->SetBucketPageId(i, image_bucket_page_id);
      dir_page->SetLocalDepth(i, dir_page->GetLocalDepth(target_bucket_index));
    }
  }

  // 尝试收缩Directory 即目录索引可以减少 (参与运算的低位可以减少)
  // 这里要循环，不能只收缩一次
  while (dir_page->CanShrink()) {
    dir_page->DecrGlobalDepth();
  }

  assert(buffer_pool_manager_->UnpinPage(dir_page->GetPageId(), true));
  table_latch_.WUnlock();
}

/*****************************************************************************
 * GETGLOBALDEPTH - DO NOT TOUCH
 *****************************************************************************/
template <typename KeyType, typename ValueType, typename KeyComparator>
uint32_t HASH_TABLE_TYPE::GetGlobalDepth() {
  table_latch_.RLock();
  HashTableDirectoryPage *dir_page = FetchDirectoryPage();
  uint32_t global_depth = dir_page->GetGlobalDepth();
  assert(buffer_pool_manager_->UnpinPage(directory_page_id_, false, nullptr));
  table_latch_.RUnlock();
  return global_depth;
}

/*****************************************************************************
 * VERIFY INTEGRITY - DO NOT TOUCH
 *****************************************************************************/
template <typename KeyType, typename ValueType, typename KeyComparator>
void HASH_TABLE_TYPE::VerifyIntegrity() {
  table_latch_.RLock();
  HashTableDirectoryPage *dir_page = FetchDirectoryPage();
  dir_page->VerifyIntegrity();
  assert(buffer_pool_manager_->UnpinPage(directory_page_id_, false, nullptr));
  table_latch_.RUnlock();
}

/*****************************************************************************
 * TEMPLATE DEFINITIONS - DO NOT TOUCH
 *****************************************************************************/
template class ExtendibleHashTable<int, int, IntComparator>;

template class ExtendibleHashTable<GenericKey<4>, RID, GenericComparator<4>>;
template class ExtendibleHashTable<GenericKey<8>, RID, GenericComparator<8>>;
template class ExtendibleHashTable<GenericKey<16>, RID, GenericComparator<16>>;
template class ExtendibleHashTable<GenericKey<32>, RID, GenericComparator<32>>;
template class ExtendibleHashTable<GenericKey<64>, RID, GenericComparator<64>>;

}  // namespace bustub
