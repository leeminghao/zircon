// Copyright 2018 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#pragma once

#include <fbl/canary.h>
#include <fbl/intrusive_double_list.h>
#include <fbl/recycler.h>
#include <fbl/ref_counted.h>
#include <fbl/ref_ptr.h>
#include <kernel/event.h>
#include <zircon/listnode.h>

// delayed memory allocation
class PageAllocRequest final :
    public fbl::Canary<fbl::magic("PGAR")>,
    public fbl::RefCounted<PageAllocRequest>,
    public fbl::Recyclable<PageAllocRequest>,
    public fbl::DoublyLinkedListable<PageAllocRequest*> {
public:
    PageAllocRequest();
    ~PageAllocRequest();

    void Clear();
    void Initialize(size_t count); // initialize a request

    static fbl::RefPtr<PageAllocRequest> GetRequest();
private:
    DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(PageAllocRequest);

    // when ref ptr goes to zero, call this
    friend class fbl::Recyclable<PageAllocRequest>;
    void fbl_recycle();

    // request info
    enum {
        FREE,
        QUEUED,
        COMPLETED,
        ERROR
    } state_ = FREE;

    size_t count_ = 0;
    list_node page_list_ = LIST_INITIAL_VALUE(page_list_);
    Event event_;
};

void page_alloc_request_init();

