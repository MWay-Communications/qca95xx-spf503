/*
 * Copyright (c) 2017 Qualcomm Innovation Center, Inc.
 * All Rights Reserved.
 * Confidential and Proprietary - Qualcomm Innovation Center, Inc.
 *
 *
 * Copyright (c) 2002-2010, Atheros Communications Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "dfs.h"
#include <ieee80211_var.h>
#ifndef UNINET
#include <ieee80211_channel.h>
#endif
#if defined(ATH_SUPPORT_DFS) && !defined(ATH_DFS_RADAR_DETECTION_ONLY)
#include "dfs_ioctl.h"
#include "dfs_ioctl_private.h"
#include "qdf_time.h" /* qdf_time_t, qdf_time_after */

static void
dfs_nol_delete(struct ath_dfs *dfs, u_int16_t delfreq, u_int16_t delchwidth);

static
OS_TIMER_FUNC(dfs_remove_from_nol)
{
    struct dfs_nol_timer_arg *nol_arg;
    struct ath_dfs *dfs;
    struct ieee80211com *ic;
    u_int16_t delfreq;
    u_int16_t delchwidth;
    struct ieee80211_channel *ptarget_channel = NULL;
    int target_channel = -1;

    OS_GET_TIMER_ARG(nol_arg, struct dfs_nol_timer_arg *);

    dfs = nol_arg->dfs;
    ic = dfs->ic;
    delfreq = nol_arg->delfreq;
    delchwidth = nol_arg->delchwidth;

    /* Only operate in HOSTAP/IBSS */
    if (ic->ic_opmode != IEEE80211_M_HOSTAP &&
            ic->ic_opmode != IEEE80211_M_STA &&
            ic->ic_opmode != IEEE80211_M_IBSS)
        goto done;

    ATH_DFSNOL_LOCK(dfs);
    /* Delete the given NOL entry */
    dfs_nol_delete(dfs, delfreq, delchwidth);
    /* Update the wireless stack with the new NOL */
    dfs_nol_update(dfs);
    ATH_DFSNOL_UNLOCK(dfs);

    if (ic->no_chans_available == 1) {
        target_channel = ic->ic_random_channel(ic, IEEE80211_SELECT_NONDFS_AND_DFS, IEEE80211_SELECT_NXT_CH_POST_RADAR_DETECT);
        if ( target_channel != -1) {
            ptarget_channel = &ic->ic_channels[target_channel];
            ic->ic_curchan = ptarget_channel;
            ic->no_chans_available = 0;
            ic->ic_bringup_ap_vaps(ic);
        } else {
            ic->no_chans_available = 1;
        }
    }

done:
    OS_FREE(nol_arg);
    return;
}

u_int8_t dfs_is_channel_in_nol(struct ath_dfs *dfs, u_int32_t chan_freq)
{
    struct dfs_nolelem *nol;

    if (dfs == NULL) {
        return 0;
    }

    ATH_DFSNOL_LOCK(dfs);
    nol = dfs->dfs_nol;

    while (nol != NULL) {
        if (nol->nol_freq == chan_freq) {
            ATH_DFSNOL_UNLOCK(dfs);
            return 1;
        }
        nol = nol->nol_next;
    }

    ATH_DFSNOL_UNLOCK(dfs);
    return 0;
}
EXPORT_SYMBOL(dfs_is_channel_in_nol);

void
dfs_print_nol(struct ath_dfs *dfs)
{
    struct dfs_nolelem *nol;
    int i = 0;
    uint32_t diff_ms, remaining_sec;

    if (dfs == NULL) {
        DFS_DPRINTK(dfs, ATH_DEBUG_DFS_NOL, "%s: sc_dfs is NULL\n", __func__);
        return;
    }
    nol = dfs->dfs_nol;
    DFS_DPRINTK(dfs, ATH_DEBUG_DFS_NOL, "%s: NOL\n", __func__);
    while (nol != NULL) {
        diff_ms = qdf_system_ticks_to_msecs(qdf_system_ticks() - nol->nol_start_ticks);
        diff_ms = (nol->nol_timeout_ms - diff_ms);
        remaining_sec = diff_ms / 1000; /* convert to seconds */
        QDF_PRINT_INFO(QDF_PRINT_IDX_SHARED, QDF_MODULE_ID_ANY, QDF_TRACE_LEVEL_INFO, "nol:%d channel=%d MHz width=%d MHz time left=%u seconds nol starttick=%llu \n",
            i++, nol->nol_freq, nol->nol_chwidth, remaining_sec,
            (unsigned long long)nol->nol_start_ticks);
        nol = nol->nol_next;
    }
}

#if ATH_SUPPORT_DFS && ATH_SUPPORT_STA_DFS
void
dfs_print_nolhistory(struct ieee80211com *ic,struct ath_dfs *dfs)
{

    if (dfs == NULL) {
        DFS_DPRINTK(dfs, ATH_DEBUG_DFS_NOL, "%s: sc_dfs is NULL\n", __func__);
        return;
    }
    ic->ic_dfs_print_nolhistory(ic);
    return;
}
#endif

#if ATH_SUPPORT_ZERO_CAC_DFS
void
dfs_print_precaclists(struct ieee80211com *ic,struct ath_dfs *dfs)
{

    if (dfs == NULL) {
        DFS_DPRINTK(dfs, ATH_DEBUG_DFS_NOL, "%s: sc_dfs is NULL\n", __func__);
        return;
    }
    ic->ic_dfs_print_precaclists(ic);
    return;
}

void
dfs_reset_precaclists(struct ieee80211com *ic,struct ath_dfs *dfs)
{

    if (dfs == NULL) {
        DFS_DPRINTK(dfs, ATH_DEBUG_DFS_NOL, "%s: sc_dfs is NULL\n", __func__);
        return;
    }
    ic->ic_dfs_reset_precaclists(ic);
    return;
}
#endif

void
dfs_get_nol(struct ath_dfs *dfs, struct dfsreq_nolelem *dfs_nol,
    int *nchan)
{
    struct dfs_nolelem *nol;

    *nchan = 0;

    if (dfs == NULL) {
        DFS_DPRINTK(dfs, ATH_DEBUG_DFS_NOL, "%s: sc_dfs is NULL\n", __func__);
        return;
    }

    nol = dfs->dfs_nol;
    while (nol != NULL) {
        dfs_nol[*nchan].nol_freq = nol->nol_freq;
        dfs_nol[*nchan].nol_chwidth = nol->nol_chwidth;
        dfs_nol[*nchan].nol_start_ticks = nol->nol_start_ticks;
        dfs_nol[*nchan].nol_timeout_ms = nol->nol_timeout_ms;
        ++(*nchan);
        nol = nol->nol_next;
    }
}


void
dfs_set_nol(struct ath_dfs *dfs, struct dfsreq_nolelem *dfs_nol, int nchan)
{
#define TIME_IN_MS 1000
    u_int32_t nol_time_left_ms;
    struct ieee80211_channel chan;
    int i;

    if (dfs == NULL) {
        DFS_DPRINTK(dfs, ATH_DEBUG_DFS_NOL, "%s: sc_dfs is NULL\n", __func__);
        return;
    }

    for (i = 0; i < nchan; i++)
    {
        nol_time_left_ms = qdf_system_ticks_to_msecs(qdf_system_ticks() - dfs_nol[i].nol_start_ticks);
        if (nol_time_left_ms < dfs_nol[i].nol_timeout_ms) {
            chan.ic_freq = dfs_nol[i].nol_freq;
            chan.ic_flags= 0;
            chan.ic_flagext = 0;
            nol_time_left_ms = (dfs_nol[i].nol_timeout_ms - nol_time_left_ms);
            dfs_nol_addchan(dfs, &chan, (nol_time_left_ms / TIME_IN_MS));
        }
    }
#undef TIME_IN_MS
    dfs_nol_update(dfs);
}


void
dfs_nol_addchan(struct ath_dfs *dfs, struct ieee80211_channel *chan,
    u_int32_t dfs_nol_timeout)
{
#define TIME_IN_MS 1000
#define TIME_IN_US (TIME_IN_MS * 1000)
    struct dfs_nolelem *nol, *elem, *prev;
    struct dfs_nol_timer_arg *dfs_nol_arg;
    /* XXX for now, assume all events are 20MHz wide */
    int ch_width = 20;

    if (dfs == NULL) {
        DFS_DPRINTK(dfs, ATH_DEBUG_DFS_NOL, "%s: sc_dfs is NULL\n", __func__);
        return;
    }
    nol = dfs->dfs_nol;
    prev = dfs->dfs_nol;
    elem = NULL;
    while (nol != NULL) {
        if ((nol->nol_freq == chan->ic_freq) &&
            (nol->nol_chwidth == ch_width)) {
                nol->nol_start_ticks = qdf_system_ticks();
                nol->nol_timeout_ms = dfs_nol_timeout*TIME_IN_MS;
                DFS_DPRINTK(dfs, ATH_DEBUG_DFS_NOL,
                    "%s: Update OS Ticks for NOL %d MHz / %d MHz\n",
                    __func__, nol->nol_freq, nol->nol_chwidth);
                OS_CANCEL_TIMER(&nol->nol_timer);
                OS_SET_TIMER(&nol->nol_timer, dfs_nol_timeout*TIME_IN_MS);
                return;
        }
        prev = nol;
        nol = nol->nol_next;
    }
    /* Add a new element to the NOL*/
    elem = (struct dfs_nolelem *)OS_MALLOC(dfs->ic->ic_osdev, sizeof(struct dfs_nolelem),GFP_ATOMIC);
    if (elem == NULL) {
        goto bad;
    }
    dfs_nol_arg = (struct dfs_nol_timer_arg *)OS_MALLOC(dfs->ic->ic_osdev,
                      sizeof(struct dfs_nol_timer_arg), GFP_ATOMIC);
    if (dfs_nol_arg == NULL) {
        OS_FREE(elem);
        goto bad;
    }
    elem->nol_freq = chan->ic_freq;
    elem->nol_chwidth = ch_width;
    elem->nol_start_ticks = qdf_system_ticks();
    elem->nol_timeout_ms = dfs_nol_timeout*TIME_IN_MS;
    elem->nol_next = NULL;
    if (prev) {
        prev->nol_next = elem;
    } else {
        /* This is the first element in the NOL */
        dfs->dfs_nol = elem;
    }
    dfs_nol_arg->dfs = dfs;
    dfs_nol_arg->delfreq = elem->nol_freq;
    dfs_nol_arg->delchwidth = elem->nol_chwidth;

    OS_INIT_TIMER(dfs->ic->ic_osdev, &elem->nol_timer, dfs_remove_from_nol,
      dfs_nol_arg, QDF_TIMER_TYPE_WAKE_APPS);
    OS_SET_TIMER(&elem->nol_timer, dfs_nol_timeout*TIME_IN_MS);

    /* Update the NOL counter */
    dfs->dfs_nol_count++;

    DFS_DPRINTK(dfs, ATH_DEBUG_DFS_NOL,
      "%s: new NOL channel %d MHz / %d MHz\n",
      __func__,
      elem->nol_freq,
      elem->nol_chwidth);
    return;

bad:
    DFS_DPRINTK(dfs, ATH_DEBUG_DFS_NOL | ATH_DEBUG_DFS,
                "%s: failed to allocate memory for nol entry\n", __func__);

#undef TIME_IN_MS
#undef TIME_IN_US
}

/*
 * Delete the given frequency/chwidth from the NOL.
 */
static void
dfs_nol_delete(struct ath_dfs *dfs, u_int16_t delfreq, u_int16_t delchwidth)
{
    struct dfs_nolelem *nol,**prev_next;

    if (dfs == NULL) {
        DFS_DPRINTK(dfs, ATH_DEBUG_DFS, "%s: sc_dfs is NULL\n", __func__);
        return;
    }

    DFS_DPRINTK(dfs, ATH_DEBUG_DFS_NOL,
      "%s: remove channel=%d/%d MHz from NOL\n",
      __func__,
      delfreq, delchwidth);
    prev_next = &(dfs->dfs_nol);
    nol = dfs->dfs_nol;
    while (nol != NULL) {
        if (nol->nol_freq == delfreq && nol->nol_chwidth == delchwidth) {
            *prev_next = nol->nol_next;
            DFS_DPRINTK(dfs, ATH_DEBUG_DFS_NOL,
              "%s removing channel %d/%dMHz from NOL tstamp=%d\n",
                __func__, nol->nol_freq, nol->nol_chwidth,
                (qdf_system_ticks_to_msecs(qdf_system_ticks()) / 1000));
            OS_CANCEL_TIMER(&nol->nol_timer);
            OS_FREE(nol);
            nol = NULL;
            nol = *prev_next;

            /* Update the NOL counter */
            dfs->dfs_nol_count--;

            /* Be paranoid! */
            if (dfs->dfs_nol_count < 0) {
                DFS_PRINTK("%s: dfs_nol_count < 0; eek!\n", __func__);
                dfs->dfs_nol_count = 0;
            }

        } else {
            prev_next = &(nol->nol_next);
            nol = nol->nol_next;
        }
    }
}

/*
 * Notify the driver/umac that it should update the channel radar/NOL
 * flags based on the current NOL list.
 */
void
dfs_nol_update(struct ath_dfs *dfs)
{
    struct ieee80211com *ic = dfs->ic;
    struct dfs_nol_chan_entry *dfs_nol;
    struct dfs_nolelem *nol;
    int nlen;

    /*
     * Allocate enough entries to store the NOL.
     *
     * At least on Linux (don't ask why), if you allocate a 0 entry
     * array, the returned pointer is 0x10.  Make sure you're
     * aware of this when you start debugging.
     */
    dfs_nol = (struct dfs_nol_chan_entry *)OS_MALLOC(dfs->ic->ic_osdev,
      sizeof(struct dfs_nol_chan_entry) * dfs->dfs_nol_count,
      GFP_ATOMIC);

    if (dfs_nol == NULL) {
        /*
         * XXX TODO: if this fails, just schedule a task to retry
         * updating the NOL at a later stage.  That way the NOL
         * update _DOES_ happen - hopefully the failure was just
         * temporary.
         */
        DFS_PRINTK("%s: failed to allocate NOL update memory!\n",
            __func__);
        return;
    }


    /* populate the nol array */
    nlen = 0;

    nol = dfs->dfs_nol;
    while (nol != NULL && nlen < dfs->dfs_nol_count) {
        dfs_nol[nlen].nol_chfreq = nol->nol_freq;
        dfs_nol[nlen].nol_chwidth = nol->nol_chwidth;
        dfs_nol[nlen].nol_start_ticks = nol->nol_start_ticks;
        dfs_nol[nlen].nol_timeout_ms = nol->nol_timeout_ms;
        nlen++;
        nol = nol->nol_next;
    }

    /* Be suitably paranoid for now */
    if (nlen != dfs->dfs_nol_count)
        DFS_PRINTK("%s: nlen (%d) != dfs->dfs_nol_count (%d)!\n",
            __func__, nlen, dfs->dfs_nol_count);

    /*
     * Call the driver layer to have it recalculate the NOL flags for
     * each driver/umac channel.
     *
     * If the list is empty, pass NULL instead of dfs_nol.
     *
     * The operating system may have some special representation for
     * "malloc a 0 byte memory region" - for example,
     * Linux 2.6.38-13 (ubuntu) returns 0x10 rather than a valid
     * allocation (and is likely not NULL so the pointer doesn't
     * match NULL checks in any later code.
     */
    ic->ic_dfs_clist_update(ic, DFS_NOL_CLIST_CMD_UPDATE,
      (nlen > 0) ? dfs_nol : NULL, nlen);

    /*
     * .. and we're done.
     */
    OS_FREE(dfs_nol);
}

void dfs_nol_timer_cleanup(struct ath_dfs *dfs)
{
    struct dfs_nolelem *nol = dfs->dfs_nol, *prev;

    ATH_DFSNOL_LOCK(dfs);
    /* First Cancel timer */
    while (nol) {
        OS_CANCEL_TIMER(&nol->nol_timer);
        nol = nol->nol_next;
    }
    /* Free NOL elem, don't mix this while loop with above loop */
    nol = dfs->dfs_nol;
    while (nol) {
        prev = nol;
        nol = nol->nol_next;
        OS_FREE(prev);
        /* Update the NOL counter */
        dfs->dfs_nol_count--;

        if (dfs->dfs_nol_count < 0) {
            DFS_PRINTK("%s: dfs_nol_count < 0\n", __func__);
            ASSERT(0);
        }
    }
    dfs->dfs_nol = NULL;
    dfs_nol_update(dfs);
    ATH_DFSNOL_UNLOCK(dfs);
    dfs->ic->no_chans_available = 0;
    return;
}
#endif /* defined(ATH_SUPPORT_DFS) && !defined(ATH_DFS_RADAR_DETECTION_ONLY) */
