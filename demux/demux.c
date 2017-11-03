/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <limits.h>
#include <pthread.h>

#include <math.h>

#include <sys/types.h>
#include <sys/stat.h>

#include "config.h"
#include "options/m_config.h"
#include "options/m_option.h"
#include "mpv_talloc.h"
#include "common/msg.h"
#include "common/global.h"
#include "osdep/threads.h"

#include "stream/stream.h"
#include "demux.h"
#include "timeline.h"
#include "stheader.h"
#include "cue.h"

// Demuxer list
extern const struct demuxer_desc demuxer_desc_edl;
extern const struct demuxer_desc demuxer_desc_cue;
extern const demuxer_desc_t demuxer_desc_rawaudio;
extern const demuxer_desc_t demuxer_desc_rawvideo;
extern const demuxer_desc_t demuxer_desc_tv;
extern const demuxer_desc_t demuxer_desc_mf;
extern const demuxer_desc_t demuxer_desc_matroska;
extern const demuxer_desc_t demuxer_desc_lavf;
extern const demuxer_desc_t demuxer_desc_playlist;
extern const demuxer_desc_t demuxer_desc_disc;
extern const demuxer_desc_t demuxer_desc_rar;
extern const demuxer_desc_t demuxer_desc_libarchive;
extern const demuxer_desc_t demuxer_desc_null;
extern const demuxer_desc_t demuxer_desc_timeline;

/* Please do not add any new demuxers here. If you want to implement a new
 * demuxer, add it to libavformat, except for wrappers around external
 * libraries and demuxers requiring binary support. */

const demuxer_desc_t *const demuxer_list[] = {
    &demuxer_desc_disc,
    &demuxer_desc_edl,
    &demuxer_desc_cue,
    &demuxer_desc_rawaudio,
    &demuxer_desc_rawvideo,
#if HAVE_TV
    &demuxer_desc_tv,
#endif
    &demuxer_desc_matroska,
#if HAVE_LIBARCHIVE
    &demuxer_desc_libarchive,
#endif
    &demuxer_desc_rar,
    &demuxer_desc_lavf,
    &demuxer_desc_mf,
    &demuxer_desc_playlist,
    &demuxer_desc_null,
    NULL
};

struct demux_opts {
    int max_bytes;
    int max_bytes_bw;
    double min_secs;
    int force_seekable;
    double min_secs_cache;
    int access_references;
    int seekable_cache;
    int create_ccs;
};

#define OPT_BASE_STRUCT struct demux_opts

const struct m_sub_options demux_conf = {
    .opts = (const struct m_option[]){
        OPT_DOUBLE("demuxer-readahead-secs", min_secs, M_OPT_MIN, .min = 0),
        OPT_INTRANGE("demuxer-max-bytes", max_bytes, 0, 0, INT_MAX),
        OPT_INTRANGE("demuxer-max-back-bytes", max_bytes_bw, 0, 0, INT_MAX),
        OPT_FLAG("force-seekable", force_seekable, 0),
        OPT_DOUBLE("cache-secs", min_secs_cache, M_OPT_MIN, .min = 0),
        OPT_FLAG("access-references", access_references, 0),
        OPT_FLAG("demuxer-seekable-cache", seekable_cache, 0),
        OPT_FLAG("sub-create-cc-track", create_ccs, 0),
        {0}
    },
    .size = sizeof(struct demux_opts),
    .defaults = &(const struct demux_opts){
        .max_bytes = 400 * 1024 * 1024,
        .max_bytes_bw = 0,
        .min_secs = 1.0,
        .min_secs_cache = 10.0,
        .access_references = 1,
    },
};

struct demux_internal {
    struct mp_log *log;

    // The demuxer runs potentially in another thread, so we keep two demuxer
    // structs; the real demuxer can access the shadow struct only.
    // Since demuxer and user threads both don't use locks, a third demuxer
    // struct d_buffer is used to copy data between them in a synchronized way.
    struct demuxer *d_thread;   // accessed by demuxer impl. (producer)
    struct demuxer *d_user;     // accessed by player (consumer)
    struct demuxer *d_buffer;   // protected by lock; used to sync d_user/thread

    // The lock protects the packet queues (struct demux_stream), d_buffer,
    // and the fields below.
    pthread_mutex_t lock;
    pthread_cond_t wakeup;
    pthread_t thread;

    // -- All the following fields are protected by lock.

    bool thread_terminate;
    bool threading;
    void (*wakeup_cb)(void *ctx);
    void *wakeup_cb_ctx;

    struct sh_stream **streams;
    int num_streams;

    int events;

    bool warned_queue_overflow;
    bool last_eof;              // last actual global EOF status
    bool eof;                   // whether we're in EOF state (reset for retry)
    bool idle;
    bool autoselect;
    double min_secs;
    int max_bytes;
    int max_bytes_bw;
    int seekable_cache;

    // Set if we know that we are at the start of the file. This is used to
    // avoid a redundant initial seek after enabling streams. We could just
    // allow it, but to avoid buggy seeking affecting normal playback, we don't.
    bool initial_state;

    bool tracks_switched;       // thread needs to inform demuxer of this

    bool seeking;               // there's a seek queued
    int seek_flags;             // flags for next seek (if seeking==true)
    double seek_pts;

    double ref_pts;             // assumed player position (only for track switches)

    double ts_offset;           // timestamp offset to apply to everything

    void (*run_fn)(void *);     // if non-NULL, function queued to be run on
    void *run_fn_arg;           // the thread as run_fn(run_fn_arg)

    // Cached state.
    bool force_cache_update;
    struct mp_tags *stream_metadata;
    struct stream_cache_info stream_cache_info;
    int64_t stream_size;
    // Updated during init only.
    char *stream_base_filename;
};

struct demux_stream {
    struct demux_internal *in;
    struct sh_stream *sh;
    enum stream_type type;
    // --- all fields are protected by in->lock

    // demuxer state
    bool selected;          // user wants packets from this stream
    bool active;            // try to keep at least 1 packet queued
                            // if false, this stream is disabled, or passively
                            // read (like subtitles)
    bool eof;               // end of demuxed stream? (true if all buffer empty)
    bool need_refresh;      // enabled mid-stream
    bool refreshing;
    bool correct_dts;       // packet DTS is strictly monotonically increasing
    bool correct_pos;       // packet pos is strictly monotonically increasing
    size_t fw_packs;        // number of packets in buffer (forward)
    size_t fw_bytes;        // total bytes of packets in buffer (forward)
    size_t bw_bytes;        // same as fw_bytes, but for back buffer
    int64_t last_pos;
    double last_dts;
    double last_ts;         // timestamp of the last packet added to queue
    double back_pts;        // smallest timestamp on the start of the back buffer
    struct demux_packet *queue_head;    // start of the full queue
    struct demux_packet *queue_tail;    // end of the full queue

    // reader (decoder) state (bitrate calculations are part of it because we
    // want to return the bitrate closest to the "current position")
    double base_ts;         // timestamp of the last packet returned to decoder
    double last_br_ts;      // timestamp of last packet bitrate was calculated
    size_t last_br_bytes;   // summed packet sizes since last bitrate calculation
    double bitrate;
    struct demux_packet *reader_head;   // points at current decoder position
    bool skip_to_keyframe;
    bool attached_picture_added;

    // for closed captions (demuxer_feed_caption)
    struct sh_stream *cc;
    bool ignore_eof;        // ignore stream in underrun detection
};

// Return "a", or if that is NOPTS, return "def".
#define PTS_OR_DEF(a, def) ((a) == MP_NOPTS_VALUE ? (def) : (a))
// If one of the values is NOPTS, always pick the other one.
#define MP_PTS_MIN(a, b) MPMIN(PTS_OR_DEF(a, b), PTS_OR_DEF(b, a))
#define MP_PTS_MAX(a, b) MPMAX(PTS_OR_DEF(a, b), PTS_OR_DEF(b, a))

#define MP_ADD_PTS(a, b) ((a) == MP_NOPTS_VALUE ? (a) : ((a) + (b)))

static void demuxer_sort_chapters(demuxer_t *demuxer);
static void *demux_thread(void *pctx);
static void update_cache(struct demux_internal *in);
static int cached_demux_control(struct demux_internal *in, int cmd, void *arg);
static void clear_demux_state(struct demux_internal *in);

static void ds_clear_reader_state(struct demux_stream *ds)
{
    ds->reader_head = NULL;
    ds->base_ts = ds->last_br_ts = MP_NOPTS_VALUE;
    ds->last_br_bytes = 0;
    ds->bitrate = -1;
    ds->skip_to_keyframe = false;
    ds->attached_picture_added = false;
}

static void ds_clear_demux_state(struct demux_stream *ds)
{
    ds_clear_reader_state(ds);

    demux_packet_t *dp = ds->queue_head;
    while (dp) {
        demux_packet_t *dn = dp->next;
        free_demux_packet(dp);
        dp = dn;
    }
    ds->queue_head = ds->queue_tail = NULL;

    ds->fw_packs = 0;
    ds->fw_bytes = 0;
    ds->bw_bytes = 0;
    ds->eof = false;
    ds->active = false;
    ds->refreshing = false;
    ds->need_refresh = false;
    ds->correct_dts = ds->correct_pos = true;
    ds->last_pos = -1;
    ds->last_ts = ds->last_dts = MP_NOPTS_VALUE;
    ds->back_pts = MP_NOPTS_VALUE;
}

void demux_set_ts_offset(struct demuxer *demuxer, double offset)
{
    struct demux_internal *in = demuxer->in;
    pthread_mutex_lock(&in->lock);
    in->ts_offset = offset;
    pthread_mutex_unlock(&in->lock);
}

// Allocate a new sh_stream of the given type. It either has to be released
// with talloc_free(), or added to a demuxer with demux_add_sh_stream(). You
// cannot add or read packets from the stream before it has been added.
struct sh_stream *demux_alloc_sh_stream(enum stream_type type)
{
    struct sh_stream *sh = talloc_ptrtype(NULL, sh);
    *sh = (struct sh_stream) {
        .type = type,
        .index = -1,
        .ff_index = -1,     // may be overwritten by demuxer
        .demuxer_id = -1,   // ... same
        .codec = talloc_zero(sh, struct mp_codec_params),
        .tags = talloc_zero(sh, struct mp_tags),
    };
    sh->codec->type = type;
    return sh;
}

// Add a new sh_stream to the demuxer. Note that as soon as the stream has been
// added, it must be immutable, and must not be released (this will happen when
// the demuxer is destroyed).
static void demux_add_sh_stream_locked(struct demux_internal *in,
                                       struct sh_stream *sh)
{
    assert(!sh->ds); // must not be added yet

    sh->ds = talloc(sh, struct demux_stream);
    *sh->ds = (struct demux_stream) {
        .in = in,
        .sh = sh,
        .type = sh->type,
        .selected = in->autoselect,
    };

    if (!sh->codec->codec)
        sh->codec->codec = "";

    sh->index = in->num_streams;
    if (sh->ff_index < 0)
        sh->ff_index = sh->index;
    if (sh->demuxer_id < 0) {
        sh->demuxer_id = 0;
        for (int n = 0; n < in->num_streams; n++) {
            if (in->streams[n]->type == sh->type)
                sh->demuxer_id += 1;
        }
    }

    MP_TARRAY_APPEND(in, in->streams, in->num_streams, sh);

    in->events |= DEMUX_EVENT_STREAMS;
    if (in->wakeup_cb)
        in->wakeup_cb(in->wakeup_cb_ctx);
}

// For demuxer implementations only.
void demux_add_sh_stream(struct demuxer *demuxer, struct sh_stream *sh)
{
    struct demux_internal *in = demuxer->in;
    pthread_mutex_lock(&in->lock);
    demux_add_sh_stream_locked(in, sh);
    pthread_mutex_unlock(&in->lock);
}

// Update sh->tags (lazily). This must be called by demuxers which update
// stream tags after init. (sh->tags can be accessed by the playback thread,
// which means the demuxer thread cannot write or read it directly.)
// Before init is finished, sh->tags can still be accessed freely.
// Ownership of tags goes to the function.
void demux_set_stream_tags(struct demuxer *demuxer, struct sh_stream *sh,
                           struct mp_tags *tags)
{
    struct demux_internal *in = demuxer->in;
    assert(demuxer == in->d_thread);

    if (sh->ds) {
        while (demuxer->num_update_stream_tags <= sh->index) {
            MP_TARRAY_APPEND(demuxer, demuxer->update_stream_tags,
                             demuxer->num_update_stream_tags, NULL);
        }
        talloc_free(demuxer->update_stream_tags[sh->index]);
        demuxer->update_stream_tags[sh->index] = talloc_steal(demuxer, tags);

        demux_changed(demuxer, DEMUX_EVENT_METADATA);
    } else {
        // not added yet
        talloc_free(sh->tags);
        sh->tags = talloc_steal(sh, tags);
    }
}

// Return a stream with the given index. Since streams can only be added during
// the lifetime of the demuxer, it is guaranteed that an index within the valid
// range [0, demux_get_num_stream()) always returns a valid sh_stream pointer,
// which will be valid until the demuxer is destroyed.
struct sh_stream *demux_get_stream(struct demuxer *demuxer, int index)
{
    struct demux_internal *in = demuxer->in;
    pthread_mutex_lock(&in->lock);
    assert(index >= 0 && index < in->num_streams);
    struct sh_stream *r = in->streams[index];
    pthread_mutex_unlock(&in->lock);
    return r;
}

// See demux_get_stream().
int demux_get_num_stream(struct demuxer *demuxer)
{
    struct demux_internal *in = demuxer->in;
    pthread_mutex_lock(&in->lock);
    int r = in->num_streams;
    pthread_mutex_unlock(&in->lock);
    return r;
}

void free_demuxer(demuxer_t *demuxer)
{
    if (!demuxer)
        return;
    struct demux_internal *in = demuxer->in;
    assert(demuxer == in->d_user);

    demux_stop_thread(demuxer);

    if (demuxer->desc->close)
        demuxer->desc->close(in->d_thread);

    clear_demux_state(in);

    for (int n = in->num_streams - 1; n >= 0; n--)
        talloc_free(in->streams[n]);
    pthread_mutex_destroy(&in->lock);
    pthread_cond_destroy(&in->wakeup);
    talloc_free(demuxer);
}

void free_demuxer_and_stream(struct demuxer *demuxer)
{
    if (!demuxer)
        return;
    struct stream *s = demuxer->stream;
    free_demuxer(demuxer);
    free_stream(s);
}

// Start the demuxer thread, which reads ahead packets on its own.
void demux_start_thread(struct demuxer *demuxer)
{
    struct demux_internal *in = demuxer->in;
    assert(demuxer == in->d_user);

    if (!in->threading) {
        in->threading = true;
        if (pthread_create(&in->thread, NULL, demux_thread, in))
            in->threading = false;
    }
}

void demux_stop_thread(struct demuxer *demuxer)
{
    struct demux_internal *in = demuxer->in;
    assert(demuxer == in->d_user);

    if (in->threading) {
        pthread_mutex_lock(&in->lock);
        in->thread_terminate = true;
        pthread_cond_signal(&in->wakeup);
        pthread_mutex_unlock(&in->lock);
        pthread_join(in->thread, NULL);
        in->threading = false;
        in->thread_terminate = false;
    }
}

// The demuxer thread will call cb(ctx) if there's a new packet, or EOF is reached.
void demux_set_wakeup_cb(struct demuxer *demuxer, void (*cb)(void *ctx), void *ctx)
{
    struct demux_internal *in = demuxer->in;
    pthread_mutex_lock(&in->lock);
    in->wakeup_cb = cb;
    in->wakeup_cb_ctx = ctx;
    pthread_mutex_unlock(&in->lock);
}

const char *stream_type_name(enum stream_type type)
{
    switch (type) {
    case STREAM_VIDEO:  return "video";
    case STREAM_AUDIO:  return "audio";
    case STREAM_SUB:    return "sub";
    default:            return "unknown";
    }
}

static struct sh_stream *demuxer_get_cc_track_locked(struct sh_stream *stream)
{
    struct sh_stream *sh = stream->ds->cc;

    if (!sh) {
        sh = demux_alloc_sh_stream(STREAM_SUB);
        if (!sh)
            return NULL;
        sh->codec->codec = "eia_608";
        sh->default_track = true;
        stream->ds->cc = sh;
        demux_add_sh_stream_locked(stream->ds->in, sh);
        sh->ds->ignore_eof = true;
    }

    return sh;
}

void demuxer_feed_caption(struct sh_stream *stream, demux_packet_t *dp)
{
    struct demux_internal *in = stream->ds->in;

    pthread_mutex_lock(&in->lock);
    struct sh_stream *sh = demuxer_get_cc_track_locked(stream);
    if (!sh) {
        pthread_mutex_unlock(&in->lock);
        talloc_free(dp);
        return;
    }

    dp->pts = MP_ADD_PTS(dp->pts, -in->ts_offset);
    dp->dts = MP_ADD_PTS(dp->dts, -in->ts_offset);
    pthread_mutex_unlock(&in->lock);

    demux_add_packet(sh, dp);
}

// An obscure mechanism to get stream switching to be executed faster.
// On a switch, it seeks back, and then grabs all packets that were
// "missing" from the packet queue of the newly selected stream.
// Returns MP_NOPTS_VALUE if no seek should happen.
static double get_refresh_seek_pts(struct demux_internal *in)
{
    struct demuxer *demux = in->d_thread;

    double start_ts = in->ref_pts;
    bool needed = false;
    bool normal_seek = true;
    bool refresh_possible = true;
    for (int n = 0; n < in->num_streams; n++) {
        struct demux_stream *ds = in->streams[n]->ds;

        if (!ds->selected)
            continue;

        if (ds->type == STREAM_VIDEO || ds->type == STREAM_AUDIO)
            start_ts = MP_PTS_MIN(start_ts, ds->base_ts);

        needed |= ds->need_refresh;
        // If there were no other streams selected, we can use a normal seek.
        normal_seek &= ds->need_refresh;
        ds->need_refresh = false;

        refresh_possible &= ds->correct_dts || ds->correct_pos;
    }

    if (!needed || start_ts == MP_NOPTS_VALUE || !demux->desc->seek ||
        !demux->seekable || demux->partially_seekable)
        return MP_NOPTS_VALUE;

    if (normal_seek)
        return start_ts;

    if (!refresh_possible) {
        MP_VERBOSE(in, "can't issue refresh seek\n");
        return MP_NOPTS_VALUE;
    }

    for (int n = 0; n < in->num_streams; n++) {
        struct demux_stream *ds = in->streams[n]->ds;
        // Streams which didn't have any packets yet will return all packets,
        // other streams return packets only starting from the last position.
        if (ds->last_pos != -1 || ds->last_dts != MP_NOPTS_VALUE)
            ds->refreshing |= ds->selected;
    }

    // Seek back to player's current position, with a small offset added.
    return start_ts - 1.0;
}

// Get the PTS in the keyframe range starting at or following dp. We assume
// that the minimum PTS values within a keyframe range are strictly monotonic
// increasing relative to the range after it. Since we don't assume that the
// first packet has the minimum PTS, a search within the keyframe range is done.
// This function does not assume dp->keyframe==true, because it deals with weird
// cases like apparently seeking to non-keyframes, or pruning the complete
// backbuffer, which might end up with non-keyframes even at queue start.
// The caller assumption is that the first frame decoded from this packet
// position will result in a frame with the PTS returned from this function.
// (For corner cases with non-key frames, assuming those packets are skipped.)
static double recompute_keyframe_target_pts(struct demux_packet *dp)
{
    bool in_keyframe_range = false;
    double res = MP_NOPTS_VALUE;
    while (dp) {
        if (dp->keyframe) {
            if (in_keyframe_range)
                break;
            in_keyframe_range = true;
        }
        if (in_keyframe_range) {
            double ts = PTS_OR_DEF(dp->pts, dp->dts);
            if (dp->segmented && (ts < dp->start || ts > dp->end))
                ts = MP_NOPTS_VALUE;
            res = MP_PTS_MIN(res, ts);
        }
        dp = dp->next;
    }
    return res;
}

void demux_add_packet(struct sh_stream *stream, demux_packet_t *dp)
{
    struct demux_stream *ds = stream ? stream->ds : NULL;
    if (!dp || !ds) {
        talloc_free(dp);
        return;
    }
    struct demux_internal *in = ds->in;
    pthread_mutex_lock(&in->lock);

    bool drop = ds->refreshing;
    if (ds->refreshing) {
        // Resume reading once the old position was reached (i.e. we start
        // returning packets where we left off before the refresh).
        // If it's the same position, drop, but continue normally next time.
        if (ds->correct_dts) {
            ds->refreshing = dp->dts < ds->last_dts;
        } else if (ds->correct_pos) {
            ds->refreshing = dp->pos < ds->last_pos;
        } else {
            ds->refreshing = false; // should not happen
        }
    }

    if (!ds->selected || ds->need_refresh || in->seeking || drop) {
        pthread_mutex_unlock(&in->lock);
        talloc_free(dp);
        return;
    }

    ds->correct_pos &= dp->pos >= 0 && dp->pos > ds->last_pos;
    ds->correct_dts &= dp->dts != MP_NOPTS_VALUE && dp->dts > ds->last_dts;
    ds->last_pos = dp->pos;
    ds->last_dts = dp->dts;

    dp->stream = stream->index;
    dp->next = NULL;

    // (keep in mind that even if the reader went out of data, the queue is not
    // necessarily empty due to the backbuffer)
    if (!ds->reader_head && (!ds->skip_to_keyframe || dp->keyframe)) {
        ds->reader_head = dp;
        ds->skip_to_keyframe = false;
    }

    size_t bytes = demux_packet_estimate_total_size(dp);
    if (ds->reader_head) {
        ds->fw_packs++;
        ds->fw_bytes += bytes;
    } else {
        ds->bw_bytes += bytes;
    }

    if (ds->queue_tail) {
        // next packet in stream
        ds->queue_tail->next = dp;
        ds->queue_tail = dp;
    } else {
        // first packet in stream
        ds->queue_head = ds->queue_tail = dp;
    }

    // (In theory it'd be more efficient to make this incremental.)
    if (ds->back_pts == MP_NOPTS_VALUE && dp->keyframe)
        ds->back_pts = recompute_keyframe_target_pts(ds->queue_head);

    if (!ds->ignore_eof) {
        // obviously not true anymore
        ds->eof = false;
        in->last_eof = in->eof = false;
    }

    // For video, PTS determination is not trivial, but for other media types
    // distinguishing PTS and DTS is not useful.
    if (stream->type != STREAM_VIDEO && dp->pts == MP_NOPTS_VALUE)
        dp->pts = dp->dts;

    double ts = dp->dts == MP_NOPTS_VALUE ? dp->pts : dp->dts;
    if (dp->segmented)
        ts = MP_PTS_MIN(ts, dp->end);
    if (ts != MP_NOPTS_VALUE && (ts > ds->last_ts || ts + 10 < ds->last_ts))
        ds->last_ts = ts;
    if (ds->base_ts == MP_NOPTS_VALUE)
        ds->base_ts = ds->last_ts;

    MP_DBG(in, "append packet to %s: size=%d pts=%f dts=%f pos=%"PRIi64" "
           "[num=%zd size=%zd]\n", stream_type_name(stream->type),
           dp->len, dp->pts, dp->dts, dp->pos, ds->fw_packs, ds->fw_bytes);

    // Wake up if this was the first packet after start/possible underrun.
    if (ds->in->wakeup_cb && ds->reader_head && !ds->reader_head->next)
        ds->in->wakeup_cb(ds->in->wakeup_cb_ctx);
    pthread_cond_signal(&in->wakeup);
    pthread_mutex_unlock(&in->lock);
}

// Returns true if there was "progress" (lock was released temporarily).
static bool read_packet(struct demux_internal *in)
{
    in->eof = false;
    in->idle = true;

    // Check if we need to read a new packet. We do this if all queues are below
    // the minimum, or if a stream explicitly needs new packets. Also includes
    // safe-guards against packet queue overflow.
    bool active = false, read_more = false;
    size_t bytes = 0;
    for (int n = 0; n < in->num_streams; n++) {
        struct demux_stream *ds = in->streams[n]->ds;
        active |= ds->active;
        read_more |= (ds->active && !ds->reader_head) || ds->refreshing;
        bytes += ds->fw_bytes;
        if (ds->active && ds->last_ts != MP_NOPTS_VALUE && in->min_secs > 0 &&
            ds->last_ts >= ds->base_ts)
            read_more |= ds->last_ts - ds->base_ts < in->min_secs;
    }
    MP_DBG(in, "bytes=%zd, active=%d, more=%d\n",
           bytes, active, read_more);
    if (bytes >= in->max_bytes) {
        if (!in->warned_queue_overflow) {
            in->warned_queue_overflow = true;
            MP_WARN(in, "Too many packets in the demuxer packet queues:\n");
            for (int n = 0; n < in->num_streams; n++) {
                struct demux_stream *ds = in->streams[n]->ds;
                if (ds->selected) {
                    MP_WARN(in, "  %s/%d: %zd packets, %zd bytes\n",
                            stream_type_name(ds->type), n,
                            ds->fw_packs,  ds->fw_bytes);
                }
            }
        }
        for (int n = 0; n < in->num_streams; n++) {
            struct demux_stream *ds = in->streams[n]->ds;
            bool eof = !ds->reader_head;
            if (eof && !ds->eof) {
                if (in->wakeup_cb)
                    in->wakeup_cb(in->wakeup_cb_ctx);
            }
            ds->eof |= eof;
        }
        pthread_cond_signal(&in->wakeup);
        return false;
    }

    double seek_pts = get_refresh_seek_pts(in);
    bool refresh_seek = seek_pts != MP_NOPTS_VALUE;
    read_more |= refresh_seek;

    if (!read_more)
        return false;

    // Actually read a packet. Drop the lock while doing so, because waiting
    // for disk or network I/O can take time.
    in->idle = false;
    in->initial_state = false;
    pthread_mutex_unlock(&in->lock);

    struct demuxer *demux = in->d_thread;

    if (refresh_seek) {
        MP_VERBOSE(in, "refresh seek to %f\n", seek_pts);
        demux->desc->seek(demux, seek_pts, SEEK_HR);
    }

    bool eof = true;
    if (demux->desc->fill_buffer && !demux_cancel_test(demux))
        eof = demux->desc->fill_buffer(demux) <= 0;
    update_cache(in);

    pthread_mutex_lock(&in->lock);

    if (!in->seeking) {
        if (eof) {
            for (int n = 0; n < in->num_streams; n++)
                in->streams[n]->ds->eof = true;
            // If we had EOF previously, then don't wakeup (avoids wakeup loop)
            if (!in->last_eof) {
                if (in->wakeup_cb)
                    in->wakeup_cb(in->wakeup_cb_ctx);
                pthread_cond_signal(&in->wakeup);
                MP_VERBOSE(in, "EOF reached.\n");
            }
        }
        in->eof = in->last_eof = eof;
    }
    return true;
}

static void prune_old_packets(struct demux_internal *in)
{
    size_t buffered = 0;
    for (int n = 0; n < in->num_streams; n++)
        buffered += in->streams[n]->ds->bw_bytes;

    MP_TRACE(in, "total backbuffer = %zd\n", buffered);

    // It's not clear what the ideal way to prune old packets is. For now, we
    // prune the oldest packet runs, as long as the total cache amount is too
    // big.
    while (buffered > in->max_bytes_bw) {
        double earliest_ts = MP_NOPTS_VALUE;
        int earliest_stream = -1;

        for (int n = 0; n < in->num_streams; n++) {
            struct demux_stream *ds = in->streams[n]->ds;

            if (ds->queue_head && ds->queue_head != ds->reader_head) {
                struct demux_packet *dp = ds->queue_head;
                double ts = PTS_OR_DEF(dp->dts, dp->pts);
                // Note: in obscure cases, packets might have no timestamps set,
                // in which case we still need to prune _something_.
                if (earliest_ts == MP_NOPTS_VALUE ||
                    (ts != MP_NOPTS_VALUE && ts < earliest_ts))
                {
                    earliest_ts = ts;
                    earliest_stream = n;
                }
            }
        }

        assert(earliest_stream >= 0); // incorrect accounting of "buffered"?
        struct demux_stream *ds = in->streams[earliest_stream]->ds;

        ds->back_pts = MP_NOPTS_VALUE;

        // Prune all packets until the next keyframe or reader_head. Keeping
        // those packets would not help with seeking at all, so we strictly
        // drop them.
        // In addition, we need to find the new possibly min. seek target,
        // which in the worst case could be inside the forward buffer. The fact
        // that many keyframe ranges without keyframes exist (audio packets)
        // makes this much harder.
        // Note: might be pretty inefficient for streams with many small audio
        // or subtitle packets. (All are keyframes, and selection logic runs for
        // every packet.)
        struct demux_packet *next_seek_target = NULL;
        for (struct demux_packet *dp = ds->queue_head; dp; dp = dp->next) {
            // (Has to be _after_ queue_head to drop at least 1 packet.)
            if (dp->keyframe && dp != ds->queue_head) {
                next_seek_target = dp;
                // Note that we set back_pts to this even if we leave some
                // packets before it - it will still be only viable seek target.
                ds->back_pts = recompute_keyframe_target_pts(dp);
                if (ds->back_pts != MP_NOPTS_VALUE)
                    break;
            }
        }

        while (ds->queue_head && (ds->queue_head != ds->reader_head &&
                                  ds->queue_head != next_seek_target))
        {
            struct demux_packet *dp = ds->queue_head;

            size_t bytes = demux_packet_estimate_total_size(dp);
            buffered -= bytes;
            MP_TRACE(in, "dropping backbuffer packet size %zd from stream %d\n",
                     bytes, earliest_stream);

            ds->queue_head = dp->next;
            if (!ds->queue_head)
                ds->queue_tail = NULL;
            talloc_free(dp);
            ds->bw_bytes -= bytes;
        }
    }
}

static void execute_trackswitch(struct demux_internal *in)
{
    in->tracks_switched = false;

    bool any_selected = false;
    for (int n = 0; n < in->num_streams; n++)
        any_selected |= in->streams[n]->ds->selected;

    pthread_mutex_unlock(&in->lock);

    if (in->d_thread->desc->control)
        in->d_thread->desc->control(in->d_thread, DEMUXER_CTRL_SWITCHED_TRACKS, 0);

    stream_control(in->d_thread->stream, STREAM_CTRL_SET_READAHEAD,
                   &(int){any_selected});

    pthread_mutex_lock(&in->lock);
}

static void execute_seek(struct demux_internal *in)
{
    int flags = in->seek_flags;
    double pts = in->seek_pts;
    in->seeking = false;
    in->initial_state = false;

    pthread_mutex_unlock(&in->lock);

    MP_VERBOSE(in, "execute seek (to %f flags %d)\n", pts, flags);

    if (in->d_thread->desc->seek)
        in->d_thread->desc->seek(in->d_thread, pts, flags);

    MP_VERBOSE(in, "seek done\n");

    pthread_mutex_lock(&in->lock);
}

static void *demux_thread(void *pctx)
{
    struct demux_internal *in = pctx;
    mpthread_set_name("demux");
    pthread_mutex_lock(&in->lock);
    while (!in->thread_terminate) {
        if (in->run_fn) {
            in->run_fn(in->run_fn_arg);
            in->run_fn = NULL;
            pthread_cond_signal(&in->wakeup);
            continue;
        }
        if (in->tracks_switched) {
            execute_trackswitch(in);
            continue;
        }
        if (in->seeking) {
            execute_seek(in);
            continue;
        }
        if (!in->eof) {
            if (read_packet(in))
                continue; // read_packet unlocked, so recheck conditions
        }
        if (in->force_cache_update) {
            pthread_mutex_unlock(&in->lock);
            update_cache(in);
            pthread_mutex_lock(&in->lock);
            in->force_cache_update = false;
            continue;
        }
        pthread_cond_signal(&in->wakeup);
        pthread_cond_wait(&in->wakeup, &in->lock);
    }
    pthread_mutex_unlock(&in->lock);
    return NULL;
}

static struct demux_packet *dequeue_packet(struct demux_stream *ds)
{
    if (ds->sh->attached_picture) {
        ds->eof = true;
        if (ds->attached_picture_added)
            return NULL;
        ds->attached_picture_added = true;
        struct demux_packet *pkt = demux_copy_packet(ds->sh->attached_picture);
        if (!pkt)
            abort();
        pkt->stream = ds->sh->index;
        return pkt;
    }
    if (!ds->reader_head)
        return NULL;
    struct demux_packet *pkt = ds->reader_head;
    ds->reader_head = pkt->next;

    // Update cached packet queue state.
    ds->fw_packs--;
    size_t bytes = demux_packet_estimate_total_size(pkt);
    ds->fw_bytes -= bytes;
    ds->bw_bytes += bytes;

    // The returned packet is mutated etc. and will be owned by the user.
    pkt = demux_copy_packet(pkt);
    if (!pkt)
        abort();
    pkt->next = NULL;

    double ts = PTS_OR_DEF(pkt->dts, pkt->pts);
    if (ts != MP_NOPTS_VALUE)
        ds->base_ts = ts;

    if (pkt->keyframe && ts != MP_NOPTS_VALUE) {
        // Update bitrate - only at keyframe points, because we use the
        // (possibly) reordered packet timestamps instead of realtime.
        double d = ts - ds->last_br_ts;
        if (ds->last_br_ts == MP_NOPTS_VALUE || d < 0) {
            ds->bitrate = -1;
            ds->last_br_ts = ts;
            ds->last_br_bytes = 0;
        } else if (d >= 0.5) { // a window of least 500ms for UI purposes
            ds->bitrate = ds->last_br_bytes / d;
            ds->last_br_ts = ts;
            ds->last_br_bytes = 0;
        }
    }
    ds->last_br_bytes += pkt->len;

    // This implies this function is actually called from "the" user thread.
    if (pkt->pos >= ds->in->d_user->filepos)
        ds->in->d_user->filepos = pkt->pos;

    pkt->pts = MP_ADD_PTS(pkt->pts, ds->in->ts_offset);
    pkt->dts = MP_ADD_PTS(pkt->dts, ds->in->ts_offset);

    pkt->start = MP_ADD_PTS(pkt->start, ds->in->ts_offset);
    pkt->end = MP_ADD_PTS(pkt->end, ds->in->ts_offset);

    prune_old_packets(ds->in);
    return pkt;
}

// Whether to avoid actively demuxing new packets to find a new packet on the
// given stream.
// Attached pictures (cover art) should never actively read.
// Sparse packets (Subtitles) interleaved with other non-sparse packets (video,
// audio) should never be read actively, meaning the demuxer thread does not
// try to exceed default readahead in order to find a new packet.
static bool use_lazy_packet_reading(struct demux_stream *ds)
{
    if (ds->sh->attached_picture)
        return true;
    if (ds->type != STREAM_SUB)
        return false;
    // Subtitles are only lazily read if there's at least 1 other actively read
    // stream.
    for (int n = 0; n < ds->in->num_streams; n++) {
        struct demux_stream *s = ds->in->streams[n]->ds;
        if (s->type != STREAM_SUB && s->selected && !s->eof && !
            s->sh->attached_picture)
            return true;
    }
    return false;
}

// Read a packet from the given stream. The returned packet belongs to the
// caller, who has to free it with talloc_free(). Might block. Returns NULL
// on EOF.
struct demux_packet *demux_read_packet(struct sh_stream *sh)
{
    struct demux_stream *ds = sh ? sh->ds : NULL;
    struct demux_packet *pkt = NULL;
    if (ds) {
        struct demux_internal *in = ds->in;
        pthread_mutex_lock(&in->lock);
        if (!use_lazy_packet_reading(ds)) {
            const char *t = stream_type_name(ds->type);
            MP_DBG(in, "reading packet for %s\n", t);
            in->eof = false; // force retry
            while (ds->selected && !ds->reader_head) {
                ds->active = true;
                // Note: the following code marks EOF if it can't continue
                if (in->threading) {
                    MP_VERBOSE(in, "waiting for demux thread (%s)\n", t);
                    pthread_cond_signal(&in->wakeup);
                    pthread_cond_wait(&in->wakeup, &in->lock);
                } else {
                    read_packet(in);
                }
                if (ds->eof)
                    break;
            }
        }
        pkt = dequeue_packet(ds);
        pthread_cond_signal(&in->wakeup); // possibly read more
        pthread_mutex_unlock(&in->lock);
    }
    return pkt;
}

// Poll the demuxer queue, and if there's a packet, return it. Otherwise, just
// make the demuxer thread read packets for this stream, and if there's at
// least one packet, call the wakeup callback.
// Unlike demux_read_packet(), this always enables readahead (except for
// interleaved subtitles).
// Returns:
//   < 0: EOF was reached, *out_pkt=NULL
//  == 0: no new packet yet, but maybe later, *out_pkt=NULL
//   > 0: new packet read, *out_pkt is set
// Note: when reading interleaved subtitles, the demuxer won't try to forcibly
// read ahead to get the next subtitle packet (as the next packet could be
// minutes away). In this situation, this function will just return -1.
int demux_read_packet_async(struct sh_stream *sh, struct demux_packet **out_pkt)
{
    struct demux_stream *ds = sh ? sh->ds : NULL;
    int r = -1;
    *out_pkt = NULL;
    if (ds) {
        if (ds->in->threading) {
            pthread_mutex_lock(&ds->in->lock);
            *out_pkt = dequeue_packet(ds);
            if (use_lazy_packet_reading(ds)) {
                r = *out_pkt ? 1 : -1;
            } else {
                r = *out_pkt ? 1 : ((ds->eof || !ds->selected) ? -1 : 0);
                ds->active = ds->selected; // enable readahead
                ds->in->eof = false; // force retry
                pthread_cond_signal(&ds->in->wakeup); // possibly read more
            }
            pthread_mutex_unlock(&ds->in->lock);
        } else {
            *out_pkt = demux_read_packet(sh);
            r = *out_pkt ? 1 : -1;
        }
    }
    return r;
}

// Return whether a packet is queued. Never blocks, never forces any reads.
bool demux_has_packet(struct sh_stream *sh)
{
    bool has_packet = false;
    if (sh) {
        pthread_mutex_lock(&sh->ds->in->lock);
        has_packet = sh->ds->reader_head;
        pthread_mutex_unlock(&sh->ds->in->lock);
    }
    return has_packet;
}

// Read and return any packet we find. NULL means EOF.
struct demux_packet *demux_read_any_packet(struct demuxer *demuxer)
{
    struct demux_internal *in = demuxer->in;
    assert(!in->threading); // doesn't work with threading
    bool read_more = true;
    while (read_more) {
        for (int n = 0; n < in->num_streams; n++) {
            struct sh_stream *sh = in->streams[n];
            sh->ds->active = sh->ds->selected; // force read_packet() to read
            struct demux_packet *pkt = dequeue_packet(sh->ds);
            if (pkt)
                return pkt;
        }
        // retry after calling this
        pthread_mutex_lock(&in->lock); // lock only because read_packet unlocks
        read_more = read_packet(in);
        read_more &= !in->eof;
        pthread_mutex_unlock(&in->lock);
    }
    return NULL;
}

void demuxer_help(struct mp_log *log)
{
    int i;

    mp_info(log, "Available demuxers:\n");
    mp_info(log, " demuxer:   info:\n");
    for (i = 0; demuxer_list[i]; i++) {
        mp_info(log, "%10s  %s\n",
                demuxer_list[i]->name, demuxer_list[i]->desc);
    }
}

static const char *d_level(enum demux_check level)
{
    switch (level) {
    case DEMUX_CHECK_FORCE:  return "force";
    case DEMUX_CHECK_UNSAFE: return "unsafe";
    case DEMUX_CHECK_REQUEST:return "request";
    case DEMUX_CHECK_NORMAL: return "normal";
    }
    abort();
}

static int decode_float(char *str, float *out)
{
    char *rest;
    float dec_val;

    dec_val = strtod(str, &rest);
    if (!rest || (rest == str) || !isfinite(dec_val))
        return -1;

    *out = dec_val;
    return 0;
}

static int decode_gain(struct mp_log *log, struct mp_tags *tags,
                       const char *tag, float *out)
{
    char *tag_val = NULL;
    float dec_val;

    tag_val = mp_tags_get_str(tags, tag);
    if (!tag_val)
        return -1;

    if (decode_float(tag_val, &dec_val) < 0) {
        mp_msg(log, MSGL_ERR, "Invalid replaygain value\n");
        return -1;
    }

    *out = dec_val;
    return 0;
}

static int decode_peak(struct mp_log *log, struct mp_tags *tags,
                       const char *tag, float *out)
{
    char *tag_val = NULL;
    float dec_val;

    *out = 1.0;

    tag_val = mp_tags_get_str(tags, tag);
    if (!tag_val)
        return 0;

    if (decode_float(tag_val, &dec_val) < 0 || dec_val <= 0.0)
        return -1;

    *out = dec_val;
    return 0;
}

static struct replaygain_data *decode_rgain(struct mp_log *log,
                                            struct mp_tags *tags)
{
    struct replaygain_data rg = {0};

    if (decode_gain(log, tags, "REPLAYGAIN_TRACK_GAIN", &rg.track_gain) >= 0 &&
        decode_peak(log, tags, "REPLAYGAIN_TRACK_PEAK", &rg.track_peak) >= 0)
    {
        if (decode_gain(log, tags, "REPLAYGAIN_ALBUM_GAIN", &rg.album_gain) < 0 ||
            decode_peak(log, tags, "REPLAYGAIN_ALBUM_PEAK", &rg.album_peak) < 0)
        {
            rg.album_gain = rg.track_gain;
            rg.album_peak = rg.track_peak;
        }
        return talloc_memdup(NULL, &rg, sizeof(rg));
    }

    if (decode_gain(log, tags, "REPLAYGAIN_GAIN", &rg.track_gain) >= 0 &&
        decode_peak(log, tags, "REPLAYGAIN_PEAK", &rg.track_peak) >= 0)
    {
        rg.album_gain = rg.track_gain;
        rg.album_peak = rg.track_peak;
        return talloc_memdup(NULL, &rg, sizeof(rg));
    }

    return NULL;
}

static void demux_update_replaygain(demuxer_t *demuxer)
{
    struct demux_internal *in = demuxer->in;
    for (int n = 0; n < in->num_streams; n++) {
        struct sh_stream *sh = in->streams[n];
        if (sh->type == STREAM_AUDIO && !sh->codec->replaygain_data) {
            struct replaygain_data *rg = decode_rgain(demuxer->log, sh->tags);
            if (!rg)
                rg = decode_rgain(demuxer->log, demuxer->metadata);
            if (rg)
                sh->codec->replaygain_data = talloc_steal(in, rg);
        }
    }
}

// Copy all fields from src to dst, depending on event flags.
static void demux_copy(struct demuxer *dst, struct demuxer *src)
{
    if (src->events & DEMUX_EVENT_INIT) {
        // Note that we do as shallow copies as possible. We expect the data
        // that is not-copied (only referenced) to be immutable.
        // This implies e.g. that no chapters are added after initialization.
        dst->chapters = src->chapters;
        dst->num_chapters = src->num_chapters;
        dst->editions = src->editions;
        dst->num_editions = src->num_editions;
        dst->edition = src->edition;
        dst->attachments = src->attachments;
        dst->num_attachments = src->num_attachments;
        dst->matroska_data = src->matroska_data;
        dst->playlist = src->playlist;
        dst->seekable = src->seekable;
        dst->partially_seekable = src->partially_seekable;
        dst->filetype = src->filetype;
        dst->ts_resets_possible = src->ts_resets_possible;
        dst->fully_read = src->fully_read;
        dst->start_time = src->start_time;
        dst->duration = src->duration;
        dst->is_network = src->is_network;
        dst->priv = src->priv;
    }

    if (src->events & DEMUX_EVENT_METADATA) {
        talloc_free(dst->metadata);
        dst->metadata = mp_tags_dup(dst, src->metadata);

        if (dst->num_update_stream_tags != src->num_update_stream_tags) {
            dst->num_update_stream_tags = src->num_update_stream_tags;
            talloc_free(dst->update_stream_tags);
            dst->update_stream_tags =
                talloc_zero_array(dst, struct mp_tags *, dst->num_update_stream_tags);
        }
        for (int n = 0; n < dst->num_update_stream_tags; n++) {
            talloc_free(dst->update_stream_tags[n]);
            dst->update_stream_tags[n] =
                talloc_steal(dst->update_stream_tags, src->update_stream_tags[n]);
            src->update_stream_tags[n] = NULL;
        }
    }

    dst->events |= src->events;
    src->events = 0;
}

// This is called by demuxer implementations if certain parameters change
// at runtime.
// events is one of DEMUX_EVENT_*
// The code will copy the fields references by the events to the user-thread.
void demux_changed(demuxer_t *demuxer, int events)
{
    assert(demuxer == demuxer->in->d_thread); // call from demuxer impl. only
    struct demux_internal *in = demuxer->in;

    demuxer->events |= events;

    update_cache(in);

    pthread_mutex_lock(&in->lock);

    if (demuxer->events & DEMUX_EVENT_INIT)
        demuxer_sort_chapters(demuxer);

    demux_copy(in->d_buffer, demuxer);

    if (in->wakeup_cb)
        in->wakeup_cb(in->wakeup_cb_ctx);
    pthread_mutex_unlock(&in->lock);
}

// Called by the user thread (i.e. player) to update metadata and other things
// from the demuxer thread.
void demux_update(demuxer_t *demuxer)
{
    assert(demuxer == demuxer->in->d_user);
    struct demux_internal *in = demuxer->in;

    if (!in->threading)
        update_cache(in);

    pthread_mutex_lock(&in->lock);
    demux_copy(demuxer, in->d_buffer);
    demuxer->events |= in->events;
    in->events = 0;
    if (demuxer->events & DEMUX_EVENT_METADATA) {
        int num_streams = MPMIN(in->num_streams, demuxer->num_update_stream_tags);
        for (int n = 0; n < num_streams; n++) {
            struct mp_tags *tags = demuxer->update_stream_tags[n];
            demuxer->update_stream_tags[n] = NULL;
            if (tags) {
                struct sh_stream *sh = in->streams[n];
                talloc_free(sh->tags);
                sh->tags = talloc_steal(sh, tags);
            }
        }

        // Often useful audio-only files, which have metadata in the audio track
        // metadata instead of the main metadata (especially OGG).
        if (in->num_streams == 1)
            mp_tags_merge(demuxer->metadata, in->streams[0]->tags);

        if (in->stream_metadata)
            mp_tags_merge(demuxer->metadata, in->stream_metadata);
    }
    if (demuxer->events & (DEMUX_EVENT_METADATA | DEMUX_EVENT_STREAMS))
        demux_update_replaygain(demuxer);
    pthread_mutex_unlock(&in->lock);
}

static void demux_init_cache(struct demuxer *demuxer)
{
    struct demux_internal *in = demuxer->in;
    struct stream *stream = demuxer->stream;

    char *base = NULL;
    stream_control(stream, STREAM_CTRL_GET_BASE_FILENAME, &base);
    in->stream_base_filename = talloc_steal(demuxer, base);
}

static void demux_init_cuesheet(struct demuxer *demuxer)
{
    char *cue = mp_tags_get_str(demuxer->metadata, "cuesheet");
    if (cue && !demuxer->num_chapters) {
        struct cue_file *f = mp_parse_cue(bstr0(cue));
        if (f) {
            if (mp_check_embedded_cue(f) < 0) {
                MP_WARN(demuxer, "Embedded cue sheet references more than one file. "
                        "Ignoring it.\n");
            } else {
                for (int n = 0; n < f->num_tracks; n++) {
                    struct cue_track *t = &f->tracks[n];
                    int idx = demuxer_add_chapter(demuxer, "", t->start, -1);
                    mp_tags_merge(demuxer->chapters[idx].metadata, t->tags);
                }
            }
        }
        talloc_free(f);
    }
}

static void demux_maybe_replace_stream(struct demuxer *demuxer)
{
    struct demux_internal *in = demuxer->in;
    assert(!in->threading && demuxer == in->d_user);

    if (demuxer->fully_read) {
        MP_VERBOSE(demuxer, "assuming demuxer read all data; closing stream\n");
        free_stream(demuxer->stream);
        demuxer->stream = open_memory_stream(NULL, 0); // dummy
        in->d_thread->stream = demuxer->stream;
        in->d_buffer->stream = demuxer->stream;

        if (demuxer->desc->control)
            demuxer->desc->control(in->d_thread, DEMUXER_CTRL_REPLACE_STREAM, NULL);
    }
}

static void demux_init_ccs(struct demuxer *demuxer, struct demux_opts *opts)
{
    struct demux_internal *in = demuxer->in;
    if (!opts->create_ccs)
        return;
    pthread_mutex_lock(&in->lock);
    for (int n = 0; n < in->num_streams; n++) {
        struct sh_stream *sh = in->streams[n];
        if (sh->type == STREAM_VIDEO)
            demuxer_get_cc_track_locked(sh);
    }
    pthread_mutex_unlock(&in->lock);
}

static struct demuxer *open_given_type(struct mpv_global *global,
                                       struct mp_log *log,
                                       const struct demuxer_desc *desc,
                                       struct stream *stream,
                                       struct demuxer_params *params,
                                       enum demux_check check)
{
    if (mp_cancel_test(stream->cancel))
        return NULL;

    struct demuxer *demuxer = talloc_ptrtype(NULL, demuxer);
    struct demux_opts *opts = mp_get_config_group(demuxer, global, &demux_conf);
    *demuxer = (struct demuxer) {
        .desc = desc,
        .stream = stream,
        .seekable = stream->seekable,
        .filepos = -1,
        .global = global,
        .log = mp_log_new(demuxer, log, desc->name),
        .glog = log,
        .filename = talloc_strdup(demuxer, stream->url),
        .is_network = stream->is_network,
        .access_references = opts->access_references,
        .events = DEMUX_EVENT_ALL,
    };
    demuxer->seekable = stream->seekable;
    if (demuxer->stream->underlying && !demuxer->stream->underlying->seekable)
        demuxer->seekable = false;

    struct demux_internal *in = demuxer->in = talloc_ptrtype(demuxer, in);
    *in = (struct demux_internal){
        .log = demuxer->log,
        .d_thread = talloc(demuxer, struct demuxer),
        .d_buffer = talloc(demuxer, struct demuxer),
        .d_user = demuxer,
        .min_secs = opts->min_secs,
        .max_bytes = opts->max_bytes,
        .max_bytes_bw = opts->max_bytes_bw,
        .seekable_cache = opts->seekable_cache,
        .initial_state = true,
    };
    pthread_mutex_init(&in->lock, NULL);
    pthread_cond_init(&in->wakeup, NULL);

    *in->d_thread = *demuxer;
    *in->d_buffer = *demuxer;

    in->d_thread->metadata = talloc_zero(in->d_thread, struct mp_tags);
    in->d_user->metadata = talloc_zero(in->d_user, struct mp_tags);
    in->d_buffer->metadata = talloc_zero(in->d_buffer, struct mp_tags);

    mp_dbg(log, "Trying demuxer: %s (force-level: %s)\n",
           desc->name, d_level(check));

    // not for DVD/BD/DVB in particular
    if (stream->seekable && (!params || !params->timeline))
        stream_seek(stream, 0);

    // Peek this much data to avoid that stream_read() run by some demuxers
    // will flush previous peeked data.
    stream_peek(stream, STREAM_BUFFER_SIZE);

    in->d_thread->params = params; // temporary during open()
    int ret = demuxer->desc->open(in->d_thread, check);
    if (ret >= 0) {
        in->d_thread->params = NULL;
        if (in->d_thread->filetype)
            mp_verbose(log, "Detected file format: %s (%s)\n",
                       in->d_thread->filetype, desc->desc);
        else
            mp_verbose(log, "Detected file format: %s\n", desc->desc);
        if (!in->d_thread->seekable)
            mp_verbose(log, "Stream is not seekable.\n");
        if (!in->d_thread->seekable && opts->force_seekable) {
            mp_warn(log, "Not seekable, but enabling seeking on user request.\n");
            in->d_thread->seekable = true;
            in->d_thread->partially_seekable = true;
        }
        demux_init_cuesheet(in->d_thread);
        demux_init_cache(demuxer);
        demux_init_ccs(demuxer, opts);
        demux_changed(in->d_thread, DEMUX_EVENT_ALL);
        demux_update(demuxer);
        stream_control(demuxer->stream, STREAM_CTRL_SET_READAHEAD,
                       &(int){params ? params->initial_readahead : false});
        if (!(params && params->disable_timeline)) {
            struct timeline *tl = timeline_load(global, log, demuxer);
            if (tl) {
                struct demuxer_params params2 = {0};
                params2.timeline = tl;
                struct demuxer *sub =
                    open_given_type(global, log, &demuxer_desc_timeline, stream,
                                    &params2, DEMUX_CHECK_FORCE);
                if (sub) {
                    demuxer = sub;
                } else {
                    timeline_destroy(tl);
                }
            }
        }
        if (demuxer->is_network || stream->caching)
            in->min_secs = MPMAX(in->min_secs, opts->min_secs_cache);
        return demuxer;
    }

    free_demuxer(demuxer);
    return NULL;
}

static const int d_normal[]  = {DEMUX_CHECK_NORMAL, DEMUX_CHECK_UNSAFE, -1};
static const int d_request[] = {DEMUX_CHECK_REQUEST, -1};
static const int d_force[]   = {DEMUX_CHECK_FORCE, -1};

// params can be NULL
struct demuxer *demux_open(struct stream *stream, struct demuxer_params *params,
                           struct mpv_global *global)
{
    const int *check_levels = d_normal;
    const struct demuxer_desc *check_desc = NULL;
    struct mp_log *log = mp_log_new(NULL, global->log, "!demux");
    struct demuxer *demuxer = NULL;
    char *force_format = params ? params->force_format : NULL;

    if (!force_format)
        force_format = stream->demuxer;

    if (force_format && force_format[0]) {
        check_levels = d_request;
        if (force_format[0] == '+') {
            force_format += 1;
            check_levels = d_force;
        }
        for (int n = 0; demuxer_list[n]; n++) {
            if (strcmp(demuxer_list[n]->name, force_format) == 0)
                check_desc = demuxer_list[n];
        }
        if (!check_desc) {
            mp_err(log, "Demuxer %s does not exist.\n", force_format);
            goto done;
        }
    }

    // Test demuxers from first to last, one pass for each check_levels[] entry
    for (int pass = 0; check_levels[pass] != -1; pass++) {
        enum demux_check level = check_levels[pass];
        mp_verbose(log, "Trying demuxers for level=%s.\n", d_level(level));
        for (int n = 0; demuxer_list[n]; n++) {
            const struct demuxer_desc *desc = demuxer_list[n];
            if (!check_desc || desc == check_desc) {
                demuxer = open_given_type(global, log, desc, stream, params, level);
                if (demuxer) {
                    talloc_steal(demuxer, log);
                    log = NULL;
                    goto done;
                }
            }
        }
    }

done:
    talloc_free(log);
    return demuxer;
}

// Convenience function: open the stream, enable the cache (according to params
// and global opts.), open the demuxer.
// (use free_demuxer_and_stream() to free the underlying stream too)
// Also for some reason may close the opened stream if it's not needed.
struct demuxer *demux_open_url(const char *url,
                                struct demuxer_params *params,
                                struct mp_cancel *cancel,
                                struct mpv_global *global)
{
    struct demuxer_params dummy = {0};
    if (!params)
        params = &dummy;
    struct stream *s = stream_create(url, STREAM_READ | params->stream_flags,
                                     cancel, global);
    if (!s)
        return NULL;
    if (!params->disable_cache)
        stream_enable_cache_defaults(&s);
    struct demuxer *d = demux_open(s, params, global);
    if (d) {
        demux_maybe_replace_stream(d);
    } else {
        params->demuxer_failed = true;
        free_stream(s);
    }
    return d;
}

// called locked, from user thread only
static void clear_reader_state(struct demux_internal *in)
{
    for (int n = 0; n < in->num_streams; n++)
        ds_clear_reader_state(in->streams[n]->ds);
    in->warned_queue_overflow = false;
    in->d_user->filepos = -1; // implicitly synchronized
}

static void clear_demux_state(struct demux_internal *in)
{
    clear_reader_state(in);
    for (int n = 0; n < in->num_streams; n++)
        ds_clear_demux_state(in->streams[n]->ds);
    in->eof = false;
    in->last_eof = false;
    in->idle = true;
}

// clear the packet queues
void demux_flush(demuxer_t *demuxer)
{
    pthread_mutex_lock(&demuxer->in->lock);
    clear_demux_state(demuxer->in);
    pthread_mutex_unlock(&demuxer->in->lock);
}

static void recompute_buffers(struct demux_stream *ds)
{
    ds->fw_packs = 0;
    ds->fw_bytes = 0;
    ds->bw_bytes = 0;

    bool in_backbuffer = true;
    for (struct demux_packet *dp = ds->queue_head; dp; dp = dp->next) {
        if (dp == ds->reader_head)
            in_backbuffer = false;

        size_t bytes = demux_packet_estimate_total_size(dp);
        if (in_backbuffer) {
            ds->bw_bytes += bytes;
        } else {
            ds->fw_packs++;
            ds->fw_bytes += bytes;
        }
    }
}

static struct demux_packet *find_seek_target(struct demux_stream *ds,
                                             double pts, int flags)
{
    struct demux_packet *target = NULL;
    double target_diff = MP_NOPTS_VALUE;
    for (struct demux_packet *dp = ds->queue_head; dp; dp = dp->next) {
        if (!dp->keyframe)
            continue;

        double range_pts = recompute_keyframe_target_pts(dp);
        if (range_pts == MP_NOPTS_VALUE)
            continue;

        double diff = range_pts - pts;
        if (flags & SEEK_FORWARD) {
            diff = -diff;
            if (diff > 0)
                continue;
        }
        if (target_diff != MP_NOPTS_VALUE) {
            if (diff <= 0) {
                if (target_diff <= 0 && diff <= target_diff)
                    continue;
            } else if (diff >= target_diff)
                continue;
        }
        target_diff = diff;
        target = dp;
    }

    return target;
}

// must be called locked
static bool try_seek_cache(struct demux_internal *in, double pts, int flags)
{
    if ((flags & SEEK_FACTOR) || !in->seekable_cache)
        return false;

    // no idea how this could interact
    if (in->seeking)
        return false;

    struct demux_ctrl_reader_state rstate;
    if (cached_demux_control(in, DEMUXER_CTRL_GET_READER_STATE, &rstate) < 0)
        return false;

    struct demux_seek_range r = {MP_NOPTS_VALUE, MP_NOPTS_VALUE};
    if (rstate.num_seek_ranges > 0)
        r = rstate.seek_ranges[0];

    r.start = MP_ADD_PTS(r.start, -in->ts_offset);
    r.end = MP_ADD_PTS(r.end, -in->ts_offset);

    MP_VERBOSE(in, "in-cache seek range = %f <-> %f (%f)\n", r.start, r.end, pts);

    if (pts < r.start || pts > r.end)
        return false;

    clear_reader_state(in);

    // Adjust the seek target to the found video key frames. Otherwise the
    // video will undershoot the seek target, while audio will be closer to it.
    // The player frontend will play the additional video without audio, so
    // you get silent audio for the amount of "undershoot". Adjusting the seek
    // target will make the audio seek to the video target or before.
    // (If hr-seeks are used, it's better to skip this, as it would only mean
    // that more audio data than necessary would have to be decoded.)
    if (!(flags & SEEK_HR)) {
        for (int n = 0; n < in->num_streams; n++) {
            struct demux_stream *ds = in->streams[n]->ds;
            if (ds->selected && ds->type == STREAM_VIDEO) {
                struct demux_packet *target = find_seek_target(ds, pts, flags);
                if (target) {
                    double target_pts = recompute_keyframe_target_pts(target);
                    if (target_pts != MP_NOPTS_VALUE) {
                        MP_VERBOSE(in, "adjust seek target %f -> %f\n",
                                   pts, target_pts);
                        // (We assume the find_seek_target() will return the
                        // same target for the video stream.)
                        pts = target_pts;
                        flags &= ~SEEK_FORWARD;
                    }
                }
                break;
            }
        }
    }

    for (int n = 0; n < in->num_streams; n++) {
        struct demux_stream *ds = in->streams[n]->ds;

        struct demux_packet *target = find_seek_target(ds, pts, flags);
        ds->reader_head = target;
        ds->skip_to_keyframe = !target;
        recompute_buffers(ds);

        MP_VERBOSE(in, "seeking stream %d (%s) to ",
                   n, stream_type_name(ds->type));

        if (target) {
            MP_VERBOSE(in, "packet %f/%f\n", target->pts, target->dts);
        } else {
            MP_VERBOSE(in, "nothing\n");
        }
    }

    return true;
}

int demux_seek(demuxer_t *demuxer, double seek_pts, int flags)
{
    struct demux_internal *in = demuxer->in;
    assert(demuxer == in->d_user);

    if (!demuxer->seekable) {
        MP_WARN(demuxer, "Cannot seek in this file.\n");
        return 0;
    }

    if (seek_pts == MP_NOPTS_VALUE)
        return 0;

    pthread_mutex_lock(&in->lock);

    MP_VERBOSE(in, "queuing seek to %f%s\n", seek_pts,
               in->seeking ? " (cascade)" : "");

    if (!(flags & SEEK_FACTOR))
        seek_pts = MP_ADD_PTS(seek_pts, -in->ts_offset);

    if (try_seek_cache(in, seek_pts, flags)) {
        MP_VERBOSE(in, "in-cache seek worked!\n");
    } else {
        clear_demux_state(in);

        in->seeking = true;
        in->seek_flags = flags;
        in->seek_pts = seek_pts;

        if (!in->threading)
            execute_seek(in);
    }

    pthread_cond_signal(&in->wakeup);
    pthread_mutex_unlock(&in->lock);

    return 1;
}

struct sh_stream *demuxer_stream_by_demuxer_id(struct demuxer *d,
                                               enum stream_type t, int id)
{
    int num = demux_get_num_stream(d);
    for (int n = 0; n < num; n++) {
        struct sh_stream *s = demux_get_stream(d, n);
        if (s->type == t && s->demuxer_id == id)
            return s;
    }
    return NULL;
}

// Set whether the given stream should return packets.
// ref_pts is used only if the stream is enabled. Then it serves as approximate
// start pts for this stream (in the worst case it is ignored).
void demuxer_select_track(struct demuxer *demuxer, struct sh_stream *stream,
                          double ref_pts, bool selected)
{
    struct demux_internal *in = demuxer->in;
    pthread_mutex_lock(&in->lock);
    // don't flush buffers if stream is already selected / unselected
    if (stream->ds->selected != selected) {
        stream->ds->selected = selected;
        ds_clear_demux_state(stream->ds);
        in->tracks_switched = true;
        stream->ds->need_refresh = selected && !in->initial_state;
        if (stream->ds->need_refresh)
            in->ref_pts = MP_ADD_PTS(ref_pts, -in->ts_offset);
        if (in->threading) {
            pthread_cond_signal(&in->wakeup);
        } else {
            execute_trackswitch(in);
        }
    }
    pthread_mutex_unlock(&in->lock);
}

void demux_set_stream_autoselect(struct demuxer *demuxer, bool autoselect)
{
    assert(!demuxer->in->threading); // laziness
    demuxer->in->autoselect = autoselect;
}

// This is for demuxer implementations only. demuxer_select_track() sets the
// logical state, while this function returns the actual state (in case the
// demuxer attempts to cache even unselected packets for track switching - this
// will potentially be done in the future).
bool demux_stream_is_selected(struct sh_stream *stream)
{
    if (!stream)
        return false;
    bool r = false;
    pthread_mutex_lock(&stream->ds->in->lock);
    r = stream->ds->selected;
    pthread_mutex_unlock(&stream->ds->in->lock);
    return r;
}

int demuxer_add_attachment(demuxer_t *demuxer, char *name, char *type,
                           void *data, size_t data_size)
{
    if (!(demuxer->num_attachments % 32))
        demuxer->attachments = talloc_realloc(demuxer, demuxer->attachments,
                                              struct demux_attachment,
                                              demuxer->num_attachments + 32);

    struct demux_attachment *att = &demuxer->attachments[demuxer->num_attachments];
    att->name = talloc_strdup(demuxer->attachments, name);
    att->type = talloc_strdup(demuxer->attachments, type);
    att->data = talloc_memdup(demuxer->attachments, data, data_size);
    att->data_size = data_size;

    return demuxer->num_attachments++;
}

static int chapter_compare(const void *p1, const void *p2)
{
    struct demux_chapter *c1 = (void *)p1;
    struct demux_chapter *c2 = (void *)p2;

    if (c1->pts > c2->pts)
        return 1;
    else if (c1->pts < c2->pts)
        return -1;
    return c1->original_index > c2->original_index ? 1 :-1; // never equal
}

static void demuxer_sort_chapters(demuxer_t *demuxer)
{
    qsort(demuxer->chapters, demuxer->num_chapters,
          sizeof(struct demux_chapter), chapter_compare);
}

int demuxer_add_chapter(demuxer_t *demuxer, char *name,
                        double pts, uint64_t demuxer_id)
{
    struct demux_chapter new = {
        .original_index = demuxer->num_chapters,
        .pts = pts,
        .metadata = talloc_zero(demuxer, struct mp_tags),
        .demuxer_id = demuxer_id,
    };
    mp_tags_set_str(new.metadata, "TITLE", name);
    MP_TARRAY_APPEND(demuxer, demuxer->chapters, demuxer->num_chapters, new);
    return demuxer->num_chapters - 1;
}

// must be called not locked
static void update_cache(struct demux_internal *in)
{
    struct demuxer *demuxer = in->d_thread;
    struct stream *stream = demuxer->stream;

    // Don't lock while querying the stream.
    struct mp_tags *stream_metadata = NULL;
    struct stream_cache_info stream_cache_info = {.size = -1};

    int64_t stream_size = stream_get_size(stream);
    stream_control(stream, STREAM_CTRL_GET_METADATA, &stream_metadata);
    stream_control(stream, STREAM_CTRL_GET_CACHE_INFO, &stream_cache_info);

    pthread_mutex_lock(&in->lock);
    in->stream_size = stream_size;
    in->stream_cache_info = stream_cache_info;
    if (stream_metadata) {
        talloc_free(in->stream_metadata);
        in->stream_metadata = talloc_steal(in, stream_metadata);
        in->d_buffer->events |= DEMUX_EVENT_METADATA;
    }
    pthread_mutex_unlock(&in->lock);
}

// must be called locked
static int cached_stream_control(struct demux_internal *in, int cmd, void *arg)
{
    // If the cache is active, wake up the thread to possibly update cache state.
    if (in->stream_cache_info.size >= 0) {
        in->force_cache_update = true;
        pthread_cond_signal(&in->wakeup);
    }

    switch (cmd) {
    case STREAM_CTRL_GET_CACHE_INFO:
        if (in->stream_cache_info.size < 0)
            return STREAM_UNSUPPORTED;
        *(struct stream_cache_info *)arg = in->stream_cache_info;
        return STREAM_OK;
    case STREAM_CTRL_GET_SIZE:
        if (in->stream_size < 0)
            return STREAM_UNSUPPORTED;
        *(int64_t *)arg = in->stream_size;
        return STREAM_OK;
    case STREAM_CTRL_GET_BASE_FILENAME:
        if (!in->stream_base_filename)
            return STREAM_UNSUPPORTED;
        *(char **)arg = talloc_strdup(NULL, in->stream_base_filename);
        return STREAM_OK;
    }
    return STREAM_ERROR;
}

// must be called locked
static int cached_demux_control(struct demux_internal *in, int cmd, void *arg)
{
    switch (cmd) {
    case DEMUXER_CTRL_STREAM_CTRL: {
        struct demux_ctrl_stream_ctrl *c = arg;
        int r = cached_stream_control(in, c->ctrl, c->arg);
        if (r == STREAM_ERROR)
            break;
        c->res = r;
        return CONTROL_OK;
    }
    case DEMUXER_CTRL_GET_BITRATE_STATS: {
        double *rates = arg;
        for (int n = 0; n < STREAM_TYPE_COUNT; n++)
            rates[n] = -1;
        for (int n = 0; n < in->num_streams; n++) {
            struct demux_stream *ds = in->streams[n]->ds;
            if (ds->selected && ds->bitrate >= 0)
                rates[ds->type] = MPMAX(0, rates[ds->type]) + ds->bitrate;
        }
        return CONTROL_OK;
    }
    case DEMUXER_CTRL_GET_READER_STATE: {
        struct demux_ctrl_reader_state *r = arg;
        *r = (struct demux_ctrl_reader_state){
            .eof = in->last_eof,
            .ts_reader = MP_NOPTS_VALUE,
            .ts_duration = -1,
        };
        bool any_packets = false;
        bool seek_ok = in->seekable_cache && !in->seeking;
        double ts_min = MP_NOPTS_VALUE;
        double ts_max = MP_NOPTS_VALUE;
        for (int n = 0; n < in->num_streams; n++) {
            struct demux_stream *ds = in->streams[n]->ds;
            if (ds->active && !(!ds->queue_head && ds->eof) && !ds->ignore_eof)
            {
                r->underrun |= !ds->reader_head && !ds->eof;
                r->ts_reader = MP_PTS_MAX(r->ts_reader, ds->base_ts);
                // (yes, this is asymmetric, and uses MAX in both cases - it's ok
                // if it's a bit off for ts_max, as the demuxer can just wait for
                // new packets if we seek there and also last_ts is the highest
                // DTS or PTS, while ts_min should be as accurate as possible, as
                // we would have to trigger a real seek if it's off and we seeked
                // there)
                ts_min = MP_PTS_MAX(ts_min, ds->back_pts);
                ts_max = MP_PTS_MAX(ts_max, ds->last_ts);
                if (ds->back_pts == MP_NOPTS_VALUE ||
                    ds->last_ts == MP_NOPTS_VALUE)
                    seek_ok = false;
                any_packets |= !!ds->queue_head;
            }
        }
        r->idle = (in->idle && !r->underrun) || r->eof;
        r->underrun &= !r->idle;
        ts_min = MP_ADD_PTS(ts_min, in->ts_offset);
        ts_max = MP_ADD_PTS(ts_max, in->ts_offset);
        r->ts_reader = MP_ADD_PTS(r->ts_reader, in->ts_offset);
        if (r->ts_reader != MP_NOPTS_VALUE && r->ts_reader <= ts_max)
            r->ts_duration = ts_max - r->ts_reader;
        if (in->seeking || !any_packets)
            r->ts_duration = 0;
        if (seek_ok && ts_min != MP_NOPTS_VALUE && ts_max > ts_min) {
            r->num_seek_ranges = 1;
            r->seek_ranges[0] = (struct demux_seek_range){
                .start = ts_min,
                .end = ts_max,
            };
        }
        r->ts_end = ts_max;
        return CONTROL_OK;
    }
    }
    return CONTROL_UNKNOWN;
}

struct demux_control_args {
    struct demuxer *demuxer;
    int cmd;
    void *arg;
    int *r;
};

static void thread_demux_control(void *p)
{
    struct demux_control_args *args = p;
    struct demuxer *demuxer = args->demuxer;
    int cmd = args->cmd;
    void *arg = args->arg;
    struct demux_internal *in = demuxer->in;
    int r = CONTROL_UNKNOWN;

    if (cmd == DEMUXER_CTRL_STREAM_CTRL) {
        struct demux_ctrl_stream_ctrl *c = arg;
        if (in->threading)
            MP_VERBOSE(demuxer, "blocking for STREAM_CTRL %d\n", c->ctrl);
        c->res = stream_control(demuxer->stream, c->ctrl, c->arg);
        if (c->res != STREAM_UNSUPPORTED)
            r = CONTROL_OK;
    }
    if (r != CONTROL_OK) {
        if (in->threading)
            MP_VERBOSE(demuxer, "blocking for DEMUXER_CTRL %d\n", cmd);
        if (demuxer->desc->control)
            r = demuxer->desc->control(demuxer->in->d_thread, cmd, arg);
    }

    *args->r = r;
}

int demux_control(demuxer_t *demuxer, int cmd, void *arg)
{
    struct demux_internal *in = demuxer->in;
    assert(demuxer == in->d_user);

    if (in->threading) {
        pthread_mutex_lock(&in->lock);
        int cr = cached_demux_control(in, cmd, arg);
        pthread_mutex_unlock(&in->lock);
        if (cr != CONTROL_UNKNOWN)
            return cr;
    }

    int r = 0;
    struct demux_control_args args = {demuxer, cmd, arg, &r};
    if (in->threading) {
        MP_VERBOSE(in, "blocking on demuxer thread\n");
        pthread_mutex_lock(&in->lock);
        while (in->run_fn)
            pthread_cond_wait(&in->wakeup, &in->lock);
        in->run_fn = thread_demux_control;
        in->run_fn_arg = &args;
        pthread_cond_signal(&in->wakeup);
        while (in->run_fn)
            pthread_cond_wait(&in->wakeup, &in->lock);
        pthread_mutex_unlock(&in->lock);
    } else {
        thread_demux_control(&args);
    }

    return r;
}

int demux_stream_control(demuxer_t *demuxer, int ctrl, void *arg)
{
    struct demux_ctrl_stream_ctrl c = {ctrl, arg, STREAM_UNSUPPORTED};
    demux_control(demuxer, DEMUXER_CTRL_STREAM_CTRL, &c);
    return c.res;
}

bool demux_cancel_test(struct demuxer *demuxer)
{
    return mp_cancel_test(demuxer->stream->cancel);
}

struct demux_chapter *demux_copy_chapter_data(struct demux_chapter *c, int num)
{
    struct demux_chapter *new = talloc_array(NULL, struct demux_chapter, num);
    for (int n = 0; n < num; n++) {
        new[n] = c[n];
        new[n].metadata = mp_tags_dup(new, new[n].metadata);
    }
    return new;
}
