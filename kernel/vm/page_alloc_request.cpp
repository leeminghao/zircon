// Copyright 2018 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <vm/page_alloc_request.h>

#include <fbl/alloc_checker.h>
#include <fbl/auto_lock.h>
#include <fbl/intrusive_double_list.h>
#include <fbl/ref_ptr.h>
#include <trace.h>

#define LOCAL_TRACE 1

namespace {

// page alloctor request queue state
fbl::Mutex free_request_lock;
fbl::DoublyLinkedList<PageAllocRequest*> free_requests;
size_t free_request_count;
const size_t initial_free_request_pool = 1024;

} // namespace

void page_alloc_request_init() {
    LTRACE_ENTRY;

    // TODO: move this into an arena or other more efficient allocator
    for (size_t i = 0; i < initial_free_request_pool; i++) {
        fbl::AllocChecker ac;
        PageAllocRequest *par = new (&ac) PageAllocRequest();
        if (!ac.check()) {
            panic("error allocating page allocator pool\n");
        }

        free_requests.push_back(par);
        free_request_count++;
    }

    LTRACE_EXIT;
}

PageAllocRequest::PageAllocRequest() = default;
PageAllocRequest::~PageAllocRequest() = default;

fbl::RefPtr<PageAllocRequest> PageAllocRequest::GetRequest() {
    fbl::AutoLock guard(&free_request_lock);

    if (unlikely(free_request_count == 0)) {
        return nullptr;
    }

    auto request = free_requests.pop_front();
    free_request_count--;

    LTRACEF("returning %p count %zu\n", request, free_request_count);

    return fbl::AdoptRef<PageAllocRequest>(request);
}

void PageAllocRequest::fbl_recycle() {
    // put the request back in the cleared state
    Clear();

    fbl::AutoLock guard(&free_request_lock);

    LTRACEF("this %p: count %zu\n", this, free_request_count);

    free_requests.push_back(this);
    free_request_count++;
}

void PageAllocRequest::Clear() {
    DEBUG_ASSERT(state_ != QUEUED);
    DEBUG_ASSERT(list_is_empty(&page_list_));

    state_ = FREE;
    count_ = 0;
    event_.Unsignal();
}

void PageAllocRequest::Initialize(size_t count) {
    DEBUG_ASSERT(state_ == FREE);

    count_ = count;
}
