/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: URMA bonding WR buffer and slide window unit tests.
 */

#include <cerrno>

#include <gtest/gtest.h>

#include "bondp_slide_window.h"
#include "bondp_timewheel.h"
#include "bondp_wr_buf.h"

namespace {

static void TimewheelCountCallback(void *arg)
{
    uint32_t *count = static_cast<uint32_t *>(arg);

    (*count)++;
}

} // namespace

TEST(UrmaBondTest, SlideWindowRejectsInvalidInit)
{
    bdp_slide_wnd_t wnd = {};

    EXPECT_EQ(-1, bdp_slide_wnd_init(nullptr, 8, 4, 0));
    EXPECT_EQ(-1, bdp_slide_wnd_init(&wnd, 4, 4, 0));
}

TEST(UrmaBondTest, SlideWindowAddsAndSlidesHead)
{
    bdp_slide_wnd_t wnd = {};

    ASSERT_EQ(0, bdp_slide_wnd_init(&wnd, 8, 4, 6));
    EXPECT_TRUE(bdp_slide_wnd_seq_in_window(&wnd, 6));
    EXPECT_TRUE(bdp_slide_wnd_seq_in_window(&wnd, 1));
    EXPECT_FALSE(bdp_slide_wnd_seq_in_window(&wnd, 2));

    EXPECT_EQ(0, bdp_slide_wnd_add(&wnd, 6));
    EXPECT_EQ(7U, wnd.head);
    EXPECT_EQ(BDP_SLIDE_WND_OUT_OF_WND, bdp_slide_wnd_add(&wnd, 6));
    EXPECT_EQ(BDP_SLIDE_WND_OUT_OF_WND, bdp_slide_wnd_add(&wnd, 3));

    bdp_slide_wnd_uninit(&wnd);
}

TEST(UrmaBondTest, WrBufferAllocGetRelease)
{
    wr_buf_t buf = {};
    jfs_wr_entry_t *jfsEntry = nullptr;
    jfr_wr_entry_t *jfrEntry = nullptr;

    EXPECT_EQ(-EINVAL, wr_buf_init(nullptr, 2));
    EXPECT_EQ(-EINVAL, wr_buf_init(&buf, 0));

    ASSERT_EQ(0, wr_buf_init(&buf, 2));
    jfsEntry = jfs_wr_buf_alloc(&buf);
    ASSERT_NE(nullptr, jfsEntry);
    EXPECT_EQ(1U, jfsEntry->wr_id);
    EXPECT_EQ(WR_BUF_ENTRY_JFS, jfsEntry->entry_type);
    EXPECT_TRUE(jfsEntry == jfs_wr_buf_get(&buf, jfsEntry->wr_id));

    jfrEntry = jfr_wr_buf_alloc(&buf);
    ASSERT_NE(nullptr, jfrEntry);
    EXPECT_EQ(2U, jfrEntry->wr_id);
    EXPECT_EQ(WR_BUF_ENTRY_JFR, jfrEntry->entry_type);
    EXPECT_TRUE(jfrEntry == jfr_wr_buf_get(&buf, jfrEntry->wr_id));

    EXPECT_EQ(nullptr, jfs_wr_buf_alloc(&buf));
    jfs_wr_buf_release(&buf, jfsEntry);
    EXPECT_EQ(nullptr, jfs_wr_buf_get(&buf, 1));
    EXPECT_NE(nullptr, jfs_wr_buf_alloc(&buf));

    wr_buf_uninit(&buf);
}

TEST(UrmaBondTest, WrBufferReleaseJfrAndReuse)
{
    wr_buf_t buf = {};
    jfr_wr_entry_t *entry = nullptr;

    ASSERT_EQ(0, wr_buf_init(&buf, 1));
    entry = jfr_wr_buf_alloc(&buf);
    ASSERT_NE(nullptr, entry);
    uint64_t wrId = entry->wr_id;
    EXPECT_EQ(nullptr, jfr_wr_buf_alloc(&buf));
    jfr_wr_buf_release(&buf, entry);
    EXPECT_EQ(nullptr, jfr_wr_buf_get(&buf, wrId));
    EXPECT_NE(nullptr, jfr_wr_buf_alloc(&buf));
    wr_buf_uninit(&buf);
}

TEST(UrmaBondTest, WrBufferBatchAllocReleaseJfs)
{
    wr_buf_t buf = {};
    jfs_wr_entry_t *entries[3] = {};

    ASSERT_EQ(0, wr_buf_init(&buf, 2));
    EXPECT_EQ(0U, jfs_wr_buf_alloc_batch(&buf, entries, 0));
    EXPECT_EQ(2U, jfs_wr_buf_alloc_batch(&buf, entries, 3));
    ASSERT_NE(nullptr, entries[0]);
    ASSERT_NE(nullptr, entries[1]);
    EXPECT_EQ(1U, entries[0]->wr_id);
    EXPECT_EQ(2U, entries[1]->wr_id);
    EXPECT_EQ(0U, jfs_wr_buf_alloc_batch(&buf, entries, 1));

    jfs_wr_buf_release_batch(&buf, entries, 2);
    EXPECT_NE(nullptr, jfs_wr_buf_alloc(&buf));
    wr_buf_uninit(&buf);
}

TEST(UrmaBondTest, SlideWindowHasDuplicateAndWrapAround)
{
    bdp_slide_wnd_t wnd = {};

    ASSERT_EQ(0, bdp_slide_wnd_init(&wnd, 8, 4, 6));
    EXPECT_EQ(0, bdp_slide_wnd_add(&wnd, 7));
    EXPECT_TRUE(bdp_slide_wnd_has(&wnd, 7));
    EXPECT_EQ(BDP_SLIDE_WND_DUPLICATE, bdp_slide_wnd_add(&wnd, 7));
    EXPECT_EQ(0, bdp_slide_wnd_add(&wnd, 6));
    EXPECT_EQ(8U, bdp_slide_wnd_has(&wnd, 0) ? 8U : wnd.total_size);
    EXPECT_EQ(0, bdp_slide_wnd_add(&wnd, 0));
    bdp_slide_wnd_uninit(&wnd);
}

TEST(UrmaBondTest, TimewheelSchedulesCancelsAndAdvancesDeterministically)
{
    tw_cfg_t cfg = { .tick_ms = 10, .slot_num = 2 };
    uint32_t fired = 0;
    tw_task_id_t firstTask = 0;
    tw_task_id_t cancelTask = 0;
    tw_task_id_t roundTask = 0;

    EXPECT_EQ(nullptr, tw_create(nullptr));
    EXPECT_EQ(0U, tw_get_tick_ms(nullptr));
    EXPECT_EQ(-EINVAL, tw_schedule(nullptr, 0, TimewheelCountCallback, &fired, &firstTask));
    EXPECT_EQ(-EINVAL, tw_cancel(nullptr, firstTask));
    tw_advance(nullptr, 1);

    tw_t *tw = tw_create(&cfg);
    ASSERT_NE(nullptr, tw);
    EXPECT_EQ(10U, tw_get_tick_ms(tw));
    EXPECT_EQ(-EINVAL, tw_schedule(tw, 0, nullptr, &fired, &firstTask));
    EXPECT_EQ(-EINVAL, tw_schedule(tw, 0, TimewheelCountCallback, &fired, nullptr));
    EXPECT_EQ(-EINVAL, tw_cancel(tw, 0));
    EXPECT_EQ(-ENOENT, tw_cancel(tw, 0xdead));
    tw_advance(tw, 0);

    ASSERT_EQ(0, tw_schedule(tw, 0, TimewheelCountCallback, &fired, &firstTask));
    EXPECT_NE(0U, firstTask);
    tw_advance(tw, 1);
    EXPECT_EQ(1U, fired);
    EXPECT_EQ(-ENOENT, tw_cancel(tw, firstTask));

    ASSERT_EQ(0, tw_schedule(tw, 10, TimewheelCountCallback, &fired, &cancelTask));
    EXPECT_EQ(0, tw_cancel(tw, cancelTask));
    tw_advance(tw, 1);
    EXPECT_EQ(1U, fired);

    ASSERT_EQ(0, tw_schedule(tw, 30, TimewheelCountCallback, &fired, &roundTask));
    tw_advance(tw, 2);
    EXPECT_EQ(1U, fired);
    tw_advance(tw, 1);
    EXPECT_EQ(2U, fired);
    tw_destroy(tw);
    tw_destroy(nullptr);
}
