#include "engine.h"
#include "res.h"

static void post_log(HWND hwnd, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    char *s = (char *)malloc(strlen(buf) + 1);
    if (!s)
        return; /* heap exhaustion: drop the line, don't crash */
    strcpy(s, buf);
    PostMessageA(hwnd, WM_APP_LOG, 0, (LPARAM)s);
}

void Engine::init(HWND hwnd)
{
    if (inited_)
        return;
    inited_ = true;
    hwnd_ = hwnd;
    InitializeCriticalSection(&cs_);
    cmdEv_ = CreateEventA(NULL, FALSE, FALSE, NULL);
    audioEv_ = CreateEventA(NULL, FALSE, FALSE, NULL);
    DWORD tid = 0;
    thread_ = CreateThread(NULL, 0x100000, (LPTHREAD_START_ROUTINE)threadMain, this, 0, &tid);
}

bool Engine::shutdown()
{
    if (!inited_)
        return true;
    Cmd c;
    c.t = Cmd::QUIT;
    push(c);
    if (WaitForSingleObject(thread_, 8000) != WAIT_OBJECT_0)
        return false; /* still running: its lock, handles and buffers stay */
    CloseHandle(thread_);
    CloseHandle(cmdEv_);
    CloseHandle(audioEv_);
    delete latestRows_;
    latestRows_ = NULL;
    DeleteCriticalSection(&cs_);
    inited_ = false;
    return true;
}

void Engine::push(const Cmd &c)
{
    EnterCriticalSection(&cs_);
    cmds_.push_back(c);
    LeaveCriticalSection(&cs_);
    SetEvent(cmdEv_);
}

void Engine::load(const std::string &path, int subsong, const AudioSettings &s,
                  double startPos, bool startPaused)
{
    Cmd c;
    c.t = Cmd::LOAD;
    c.path = path;
    c.i = subsong;
    c.s = s;
    c.d = startPos;
    c.b = startPaused;
    c.gen = ++gen_;
    push(c);
}

void Engine::stop()
{
    Cmd c;
    c.t = Cmd::STOP;
    c.gen = ++gen_; /* an ENDED still in flight from before the stop is stale */
    push(c);
}

void Engine::seekTo(double seconds)
{
    Cmd c;
    c.t = Cmd::SEEK;
    c.d = seconds;
    push(c);
}

void Engine::pauseToggle()
{
    Cmd c;
    c.t = Cmd::PAUSE;
    push(c);
}

void Engine::setVolume(int vol, bool mute)
{
    Cmd c;
    c.t = Cmd::VOL;
    c.i = vol;
    c.b = mute;
    push(c);
}

void Engine::fetchRows(bool on, int half, int nch, int off)
{
    Cmd c;
    c.t = Cmd::FETCH;
    c.b = on;
    c.i = half;
    c.i2 = nch;
    c.i3 = off;
    push(c);
}

void Engine::setTrkDelay(int ms)
{
    Cmd c;
    c.t = Cmd::DELAY;
    c.i = ms;
    push(c);
}

void Engine::setRefresh(int ms)
{
    Cmd c;
    c.t = Cmd::REFRESH;
    c.i = ms;
    push(c);
}

RowsMsg *Engine::grabLatestRows()
{
    RowsMsg *m = NULL;
    EnterCriticalSection(&cs_);
    if (latestRows_)
        m = new RowsMsg(*latestRows_); /* deep copy; caller deletes */
    LeaveCriticalSection(&cs_);
    return m;
}

/* formatted note texts of one pattern column window, cached; the key packs
 * pattern/channel-offset/width so paged views get their own entry */
std::vector<std::string> &Engine::ensurePattern(int pat, int off, int nch)
{
    /* pat may be a 65534/65535 "+++"/"---" marker; off < 256, nch <= 64 */
    long long key = ((long long)pat << 20) | ((long long)off << 8) | nch;
    PatCache &pc = patCache_[key];
    if (pc.nch == nch && !pc.rows.empty())
        return pc.rows;
    int rows = mod_.patternRows(pat);
    if (rows < 0)
        rows = 0;
    if (rows > 4096) /* libopenmpt's maximum pattern length */
        rows = 4096;
    pc.rows.assign((size_t)rows, std::string());
    for (int r = 0; r < rows; r++) {
        std::string line;
        for (int ch = off; ch < off + nch; ch++) {
            std::string cell = mod_.cellText(pat, r, ch, 0 /* note column */);
            if (cell == "...")
                cell = "---";
            cell.resize(3, ' ');
            line += cell;
            line += ' ';
        }
        pc.rows[(size_t)r] = line;
    }
    pc.nch = nch;
    if (patCache_.size() > 8) {
        /* keep only this fresh entry; old windows reformat cheaply if needed */
        PatCache fresh;
        fresh.nch = pc.nch;
        fresh.rows.swap(pc.rows);
        patCache_.clear();
        patCache_[key] = fresh;
        return patCache_[key].rows;
    }
    return pc.rows;
}

std::string Engine::rowLine(int pat, int row, int off, int nch)
{
    if (pat < 0 || row < 0)
        return std::string((size_t)nch * 4, ' ');
    std::vector<std::string> &rows = ensurePattern(pat, off, nch);
    if (row >= (int)rows.size())
        return std::string((size_t)nch * 4, ' ');
    return rows[(size_t)row];
}

/* one playback row back/forward, crossing order boundaries (skipping empty
 * orders) so the current row can stay centered at pattern edges */
bool Engine::stepPrevRow(int &o, int &r)
{
    if (r > 0) {
        r--;
        return true;
    }
    for (int oo = o - 1; oo >= 0; oo--) {
        int p = mod_.orderPattern(oo);
        int n = p >= 0 ? mod_.patternRows(p) : 0;
        if (n > 0) {
            o = oo;
            r = n - 1;
            return true;
        }
    }
    return false;
}

bool Engine::stepNextRow(int &o, int &r)
{
    int p = mod_.orderPattern(o);
    int n = p >= 0 ? mod_.patternRows(p) : 0;
    if (r + 1 < n) {
        r++;
        return true;
    }
    int orders = mod_.numOrders();
    for (int oo = o + 1; oo < orders; oo++) {
        int q = mod_.orderPattern(oo);
        int m2 = q >= 0 ? mod_.patternRows(q) : 0;
        if (m2 > 0) {
            o = oo;
            r = 0;
            return true;
        }
    }
    return false;
}

/* 2*half+1 note lines centered on (order,row) */
RowsMsg *Engine::buildRowsMsg(int order, int row)
{
    const int half = trkHalf_, nch = trkNch_, off = trkOff_;
    RowsMsg *m = new RowsMsg();
    m->order = order;
    m->row = row;
    m->half = half;
    m->off = off;
    m->nch = nch;
    m->total = mod_.channels();
    /* pre-size both vectors: without this the row pushes reallocate the
     * backing array several times per build on the Win95 heap */
    m->rows.reserve((size_t)2 * half + 1);
    int o = order, r = row;
    std::vector<std::pair<int, int>> back;
    back.reserve((size_t)half);
    for (int sIdx = 0; sIdx < half; sIdx++) {
        if (stepPrevRow(o, r))
            back.push_back(std::make_pair(mod_.orderPattern(o), r));
        else
            back.push_back(std::make_pair(-1, 0));
    }
    for (int sIdx = half - 1; sIdx >= 0; sIdx--)
        m->rows.push_back(
            rowLine(back[(size_t)sIdx].first, back[(size_t)sIdx].second, off, nch));
    m->rows.push_back(rowLine(mod_.orderPattern(order), row, off, nch));
    o = order;
    r = row;
    for (int sIdx = 0; sIdx < half; sIdx++) {
        if (stepNextRow(o, r))
            m->rows.push_back(rowLine(mod_.orderPattern(o), r, off, nch));
        else
            m->rows.push_back(rowLine(-1, 0, off, nch));
    }
    return m;
}

/* called on every engine loop iteration. Picks the (order, row) at the
 * position actually heard. mod_.order()/mod_.row() describe what we last
 * rendered, which sits queued up ahead of the speakers. The heard position
 * is hardware-metered: compSamples_ sums the rendered samples of every
 * buffer the driver marks WHDR_DONE, the single in-progress buffer is
 * interpolated at the measured completion pace, and drivers that flag done
 * early fall back to a nominal-rate wall clock (see refill()). */
void Engine::maybePostRows()
{
    if (!playing_ || !mod_.isOpen())
        return;
    int o, r;
    if (rowHist_.empty()) {
        o = mod_.order();
        r = mod_.row();
    } else {
        /* heard position, hardware-metered: compSamples_ sums the exact
         * sample count of every buffer the driver has finished playing
         * (counted on the WHDR_DONE flag - no units to misread, runs at
         * the device's true pace). The single in-progress buffer is
         * interpolated by wall time, re-anchored on every completion, so
         * no error can accumulate beyond one buffer, ever. */
        if (!paused_ && !clockMode_) {
            /* in-buffer fraction at the MEASURED device pace (EMA of
             * completion spans), not the nominal rate: if the device clock
             * runs a few percent fast or slow (common in VMs), the view
             * moves at exactly the tempo the sound does. Clamped to sane
             * multiples of the nominal span, and re-anchored on every
             * completion, so the error stays within one buffer. */
            double pace = paceMs();
            long long frac = (long long)((double)(GetTickCount() - bufAnchorTick_) *
                                         (double)frames_ / pace);
            if (frac < 0)
                frac = 0;
            if (frac > (long long)frames_)
                frac = (long long)frames_;
            interpFrac_ = frac;
        }
        long long audible = clockMode_
            ? renderedBase_ + clockSamples_
            : renderedBase_ + compSamples_ + interpFrac_;
        audible -= (long long)trkDelayMs_ * rate_ / 1000;
        if (audible < 0)
            audible = 0;
        /* bracket the heard position between two observed entries; the seed
         * (row at load/seek time) acts as the entry before the first one.
         * Rows between entries are walked via the order list and the exact
         * row at the heard fraction is shown, so the highlight is never
         * quantized to buffer boundaries. */
        /* entries are in ascending sample order: binary search for the
         * first one past the heard position */
        const RowHist *prev = NULL, *next = NULL;
        std::vector<RowHist>::const_iterator it = std::upper_bound(
            rowHist_.begin(), rowHist_.end(), audible,
            [](long long a, const RowHist &h) { return a < h.samples; });
        if (it != rowHist_.end())
            next = &*it;
        if (it != rowHist_.begin())
            prev = &*(it - 1);
        long long aSamples;
        int aOrder, aRow;
        bool have = false;
        if (prev) {
            aSamples = prev->samples;
            aOrder = prev->order;
            aRow = prev->row;
            have = true;
        } else if (seedValid_) {
            aSamples = seedSamples_;
            aOrder = seedOrder_;
            aRow = seedRow_;
            have = true;
        }
        if (!have) {
            const RowHist &F = rowHist_.front();
            o = F.order;
            r = F.row;
        } else if (!next) {
            o = aOrder; /* nothing rendered ahead: end of song */
            r = aRow;
        } else {
            long long span = next->samples - aSamples;
            int window = -1;
            if (span > 0) {
                int wo = aOrder, wr = aRow;
                window = 0;
                while (window < 1024 &&
                       !(wo == next->order && wr == next->row)) {
                    if (!stepNextRow(wo, wr))
                        break;
                    window++;
                }
                if (!(wo == next->order && wr == next->row))
                    window = -1; /* pattern jump inside span: don't guess */
            }
            if (window >= 0 && span > 0) {
                long long into = audible - aSamples;
                if (into < 0)
                    into = 0;
                if (into > span)
                    into = span;
                long long steps = into * window / span;
                o = aOrder;
                r = aRow;
                for (long long k = 0; k < steps; k++)
                    if (!stepNextRow(o, r))
                        break;
            } else {
                o = aOrder;
                r = aRow;
            }
        }
    }
    EnterCriticalSection(&cs_);
    snap_.trkValid = true;
    snap_.trkOrder = o;
    snap_.trkPat = mod_.orderPattern(o);
    snap_.trkRow = r;
    LeaveCriticalSection(&cs_);
    if (!trkOn_)
        return; /* position text stays live; no pattern window to update */
    if (o == lastTrkOrder_ && r == lastTrkRow_)
        return;
    if (refreshMs_ > 0) {
        DWORD now = GetTickCount();
        if ((DWORD)(now - lastPostTick_) < (DWORD)refreshMs_)
            return; /* refresh throttle: retried on the next loop pass */
        lastPostTick_ = now;
    }
    lastTrkOrder_ = o;
    lastTrkRow_ = r;
    RowsMsg *m = buildRowsMsg(o, r);
    EnterCriticalSection(&cs_);
    delete latestRows_;
    latestRows_ = new RowsMsg(*m); /* UI can grab this synchronously */
    posts_++;
    snap_.posts = posts_;
    LeaveCriticalSection(&cs_);
    if (!PostMessageA(hwnd_, WM_APP_ROWS, 0, (LPARAM)m))
        delete m; /* queue full: latestRows_ still feeds the scroll sync */
}

EngineSnap Engine::snap()
{
    EnterCriticalSection(&cs_);
    EngineSnap s = snap_; /* played/posts are kept in snap_ under the lock */
    LeaveCriticalSection(&cs_);
    return s;
}

DWORD WINAPI Engine::threadMain(LPVOID p)
{
    try {
        ((Engine *)p)->run();
    } catch (const std::exception &e) {
        post_log(((Engine *)p)->hwnd_, "engine thread died: %s", e.what());
    } catch (...) {
        post_log(((Engine *)p)->hwnd_, "engine thread died: unknown exception");
    }
    return 0;
}

void Engine::run()
{
    /* audio render thread must preempt busy UI threads (e.g. window drags on
     * slow machines) or the waveOut buffers underrun */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    /* hoisted: the swap below ping-pongs this buffer with cmds_, so after
     * the first wake there are zero allocations on the steady-state path */
    std::vector<Cmd> todo;
    for (;;) {
      try {
        /* wake on new commands OR on a finished audio buffer (event-driven
         * refill keeps the ring topped up even when the UI thread hogs CPU) */
        HANDLE evs[2] = { cmdEv_, audioEv_ };
        WaitForMultipleObjects(2, evs, FALSE, 20);
        todo.clear();
        EnterCriticalSection(&cs_);
        todo.swap(cmds_);
        LeaveCriticalSection(&cs_);
        /* QUIT first: a command earlier in the same batch that throws must
         * not swallow it (the catch below drops the rest of the batch) */
        for (size_t i = 0; i < todo.size(); i++)
            if (todo[i].t == Cmd::QUIT)
                quit_ = true;
        if (quit_)
            break;
        bool regain = false;
        for (size_t i = 0; i < todo.size(); i++) {
            const Cmd &c = todo[i];
            switch (c.t) {
            case Cmd::QUIT:
                break;
            case Cmd::LOAD: {
                stopPlayback();
                curGen_ = c.gen;
                std::string err;
                /* file read + libopenmpt load and length scan at normal
                 * priority: nothing is playing, and a long parse at
                 * time-critical priority would freeze the UI on a 486 */
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
                bool ok = mod_.loadFile(c.path, c.i, &err);
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
                if (ok) {
                    /* interpolation filter length: 0 default, 1 none, 2 linear,
                     * 4 cubic, 8 windowed sinc (render param, not a ctl!) */
                    mod_.setRenderParam(OPENMPT_MODULE_RENDER_INTERPOLATIONFILTER_LENGTH,
                                        c.s.interpolation);
                    if (openDevice(c.s)) {
                        playing_ = true;
                        paused_ = false;
                        eof_ = false;
                        endedSent_ = false;
                        /* anchor before the first refill runs ahead */
                        seedSamples_ = totalRendered_;
                        seedOrder_ = mod_.order();
                        seedRow_ = mod_.row();
                        seedValid_ = true;
                        /* playback accounting starts fresh */
                        heardBase_ = mod_.position();
                        compSamples_ = 0;
                        interpFrac_ = 0;
                        bufAnchorTick_ = GetTickCount();
                        lastDoneTick_ = 0; /* spans across songs are gaps */
                        lastPostTick_ = 0;
                        clockSamples_ = 0;
                        clockSubmitted_ = 0;
                        clockLastTick_ = GetTickCount();
                        /* session resume: position and pause are in place
                         * before the first refill, so nothing from the song
                         * start reaches the speakers */
                        if (c.d > 0)
                            seekInternal(c.d);
                        if (c.b) {
                            paused_ = true;
                            waveOutPause(hwo_);
                            clockLastTick_ = 0;
                        }
                        post_log(hwnd_, "Loaded %s (%d ch, %d subsongs)",
                                 m95_basename(c.path).c_str(), mod_.channels(),
                                 mod_.subsongs());
                    } else {
                        failPlayback(2, "no usable sound device");
                    }
                } else {
                    post_log(hwnd_, "Load failed: %s (%s)",
                             m95_basename(c.path).c_str(), err.c_str());
                    PostMessageA(hwnd_, WM_APP_ENDED, curGen_, 1);
                }
                patCache_.clear();
                rowHist_.clear();
                lastTrkOrder_ = -1;
                lastTrkRow_ = -1;
                break;
            }
            case Cmd::STOP:
                stopPlayback();
                curGen_ = c.gen;
                break;
            case Cmd::SEEK:
                if (mod_.isOpen() && hwo_)
                    seekInternal(c.d);
                break;
            case Cmd::PAUSE:
                if (playing_ && hwo_) {
                    paused_ = !paused_;
                    if (paused_) {
                        waveOutPause(hwo_);
                        clockLastTick_ = 0; /* clock must not eat the pause */
                    } else {
                        waveOutRestart(hwo_);
                        /* resume the in-buffer fraction where it froze, at
                         * the same measured pace maybePostRows uses */
                        bufAnchorTick_ = GetTickCount() - (DWORD)(
                            frames_ ? (double)interpFrac_ * paceMs() / frames_ : 0);
                        /* the next completion span would include the pause */
                        lastDoneTick_ = 0;
                        clockLastTick_ = GetTickCount();
                    }
                }
                break;
            case Cmd::VOL:
                vol_ = c.i < 0 ? 0 : (c.i > 100 ? 100 : c.i);
                mute_ = c.b;
                setGain(mute_ ? 0 : ((long)vol_ * 256 / 100));
                regain = true; /* once per wake, however many changes came */
                break;
            case Cmd::FETCH:
                trkOn_ = c.b;
                trkHalf_ = c.i < 1 ? 1 : (c.i > 60 ? 60 : c.i);
                trkNch_ = c.i2 < 1 ? 1 : (c.i2 > 64 ? 64 : c.i2);
                trkOff_ = c.i3 < 0 ? 0 : (c.i3 > 255 ? 255 : c.i3); /* MPTM: up to 192 ch */
                lastTrkOrder_ = -1; /* force a fresh post right away */
                lastTrkRow_ = -1;
                maybePostRows();
                break;
            case Cmd::DELAY:
                trkDelayMs_ = c.i < 0 ? 0 : (c.i > 1000 ? 1000 : c.i);
                lastTrkOrder_ = -1; /* reflect the new offset right away */
                lastTrkRow_ = -1;
                maybePostRows();
                break;
            case Cmd::REFRESH:
                refreshMs_ = c.i < 0 ? 0 : (c.i > 1000 ? 1000 : c.i);
                lastPostTick_ = 0; /* apply immediately */
                break;
            }
        }
        if (regain)
            regainQueued();

        if (clockMode_ && playing_ && !paused_) {
            /* nominal-rate wall clock: advance exactly as much as wall time
             * passed; bounded by construction (no device lies involved) */
            DWORD now = GetTickCount();
            if (clockLastTick_)
                clockSamples_ += (long long)(now - clockLastTick_) * rate_ / 1000;
            clockLastTick_ = now;
        }

        if (playing_ && !paused_ && mod_.isOpen() && hwo_ && !refill())
            failPlayback(2, "sound output error");
        if (dryNow_) {
            /* behind (the CPU can't keep up): a time-critical thread that
             * never waits would starve the UI, and the user couldn't even
             * reach Settings to lower the rate. Let it breathe. */
            dryNow_ = false;
            Sleep(10);
        }

        bool allDone = true;
        for (size_t b = 0; b < bufs_.size(); b++)
            if (bufs_[b].queued)
                allDone = false;
        if (playing_ && eof_ && allDone) {
            playing_ = false;
            if (!endedSent_) {
                endedSent_ = true;
                PostMessageA(hwnd_, WM_APP_ENDED, curGen_, 0);
            }
        }

        EnterCriticalSection(&cs_);
        snap_.loaded = mod_.isOpen();
        snap_.playing = playing_;
        snap_.paused = paused_;
        snap_.pos = mod_.position();
        snap_.dur = mod_.duration();
        snap_.order = mod_.order();
        snap_.pat = mod_.pattern();
        snap_.row = mod_.row();
        snap_.speed = mod_.speed();
        snap_.bpm = mod_.bpm();
        snap_.channels = mod_.channels();
        snap_.rate = rate_;
        snap_.played = playedSec_;
        snap_.bufMs = rate_ ? (int)(frames_ * 1000 / rate_) : 0;
        /* audible position (what the speakers have had), not the render
         * position which sits queued ahead of them */
        snap_.heard = snap_.pos;
        if (playing_ && rate_ > 0)
            snap_.heard = heardBase_ +
                          (double)(clockMode_ ? clockSamples_ : compSamples_ + interpFrac_) /
                              rate_;
        LeaveCriticalSection(&cs_);
        if (playing_ && mod_.isOpen()) {
            RowHist e;
            e.samples = totalRendered_;
            e.order = snap_.order;
            e.row = snap_.row;
            if (rowHist_.empty() || rowHist_.back().samples != e.samples)
                rowHist_.push_back(e);
            /* keep what the heard position can lag behind the render:
             * the whole queue + the tracker delay + 2 s of margin (about
             * 3.3 s by default, used to be a fixed 30 s / ~1500 entries).
             * Expired entries go in blocks of 64+, one memmove per ~1.3 s
             * instead of one per 20 ms chunk; the last expired entry stays
             * as the anchor before the window. */
            long long keep = (long long)bufs_.size() * (long long)frames_ +
                             (long long)rate_ * ((trkDelayMs_ + 999) / 1000 + 2);
            long long cutoff = totalRendered_ - keep;
            std::vector<RowHist>::iterator lim = std::lower_bound(
                rowHist_.begin(), rowHist_.end(), cutoff,
                [](const RowHist &h, long long c) { return h.samples < c; });
            if (lim - rowHist_.begin() > 64)
                rowHist_.erase(rowHist_.begin(), lim - 1);
        }
        maybePostRows(); /* tracker view follows the row, engine-driven */
      } catch (const std::exception &e) {
        post_log(hwnd_, "engine: %s", e.what());
        playing_ = false;
      } catch (...) {
        post_log(hwnd_, "engine: unknown exception");
        playing_ = false;
      }
        if (quit_)
            break;
    }
    stopPlayback();
}

/* reposition playback (SEEK, and the start position of a resumed LOAD);
 * keeps the current pause state */
void Engine::seekInternal(double seconds)
{
    mod_.seek(seconds);
    waveOutReset(hwo_);
    for (size_t b = 0; b < bufs_.size(); b++)
        bufs_[b].queued = false;
    eof_ = false;
    endedSent_ = false;
    playing_ = true;
    rowHist_.clear(); /* stale rows would show briefly else */
    /* rebase: audible position is measured from here on; waveOutReset
     * returned all buffers without completing them, so nothing is counted */
    renderedBase_ = totalRendered_;
    /* the song position heard from here on starts where the seek landed
     * (renderedBase_ counts device frames) */
    heardBase_ = mod_.position();
    compSamples_ = 0;
    interpFrac_ = 0;
    bufAnchorTick_ = GetTickCount();
    lastDoneTick_ = 0; /* the next span would include the reset */
    lastPostTick_ = 0;
    clockSamples_ = 0;
    clockSubmitted_ = 0;
    clockLastTick_ = paused_ ? 0 : GetTickCount();
    /* anchor at the seek target before refill runs ahead */
    seedSamples_ = renderedBase_;
    seedOrder_ = mod_.order();
    seedRow_ = mod_.row();
    seedValid_ = true;
}

/* playback can't continue (no device, waveOut write failed): stop and tell
 * the UI, which shows it and leaves the queue stopped */
void Engine::failPlayback(int reason, const char *why)
{
    post_log(hwnd_, "Audio: %s - playback stopped", why);
    stopPlayback();
    endedSent_ = true;
    PostMessageA(hwnd_, WM_APP_ENDED, curGen_, reason);
}

/* measured duration of one buffer (EMA of completion spans), clamped to
 * sane multiples of the nominal span */
double Engine::paceMs() const
{
    double nom = rate_ ? frames_ * 1000.0 / rate_ : 1.0;
    double pace = spanEmaMs_ > 0 ? spanEmaMs_ : nom;
    if (pace < nom * 0.25)
        pace = nom * 0.25;
    if (pace > nom * 4)
        pace = nom * 4;
    return pace;
}

void Engine::stopPlayback()
{
    if (hwo_) {
        waveOutReset(hwo_);
        for (size_t b = 0; b < bufs_.size(); b++) {
            if (bufs_[b].prepared)
                waveOutUnprepareHeader(hwo_, &bufs_[b].hdr, sizeof(WAVEHDR));
            bufs_[b].prepared = false;
            bufs_[b].queued = false;
        }
        /* some drivers report still-playing right after a reset; retry so
         * a wave handle is never leaked (they are scarce on Win9x) */
        MMRESULT cr = waveOutClose(hwo_);
        for (int i = 0; cr == WAVERR_STILLPLAYING && i < 4; i++) {
            waveOutReset(hwo_);
            cr = waveOutClose(hwo_);
        }
        hwo_ = NULL;
    }
    mod_.close();
    patCache_.clear();
    rowHist_.clear();
    totalRendered_ = 0;
    renderedBase_ = 0;
    heardBase_ = 0;
    compSamples_ = 0;
    interpFrac_ = 0;
    bufAnchorTick_ = 0;
    seedValid_ = false;
    lastTrkOrder_ = -1;
    lastTrkRow_ = -1;
    EnterCriticalSection(&cs_);
    snap_.trkValid = false;
    delete latestRows_; /* a grab must not see the stopped song's rows */
    latestRows_ = NULL;
    LeaveCriticalSection(&cs_);
    playing_ = false;
    paused_ = false;
    eof_ = false;
}

bool Engine::openDevice(const AudioSettings &s)
{
    const int rates[] = { s.rate, 44100, 48000, 32000, 22050, 11025 };
    const int bits[] = { 16, 8 };
    const int chs[] = { 2, 1 };
    for (int r = 0; r < 6 && !hwo_; r++) {
        if (rates[r] <= 0)
            continue;
        bool dup = false;
        for (int q = 0; q < r; q++)
            if (rates[q] == rates[r])
                dup = true;
        if (dup)
            continue;
        for (int b = 0; b < 2 && !hwo_; b++) {
            for (int c = 0; c < 2 && !hwo_; c++) {
                WAVEFORMATEX wfx;
                memset(&wfx, 0, sizeof(wfx));
                wfx.wFormatTag = WAVE_FORMAT_PCM;
                wfx.nChannels = (WORD)chs[c];
                wfx.nSamplesPerSec = rates[r];
                wfx.wBitsPerSample = (WORD)bits[b];
                wfx.nBlockAlign = (WORD)(chs[c] * bits[b] / 8);
                wfx.nAvgBytesPerSec = rates[r] * wfx.nBlockAlign;
                /* prefer event notification so refill happens the instant a
                 * buffer drains; some drivers only accept CALLBACK_NULL */
                if (waveOutOpen(&hwo_, WAVE_MAPPER, &wfx, (DWORD_PTR)audioEv_, 0,
                                CALLBACK_EVENT) == MMSYSERR_NOERROR) {
                    eventDriven_ = true;
                    rate_ = rates[r];
                    devBits_ = bits[b];
                    devCh_ = chs[c];
                } else if (waveOutOpen(&hwo_, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) ==
                           MMSYSERR_NOERROR) {
                    eventDriven_ = false;
                    rate_ = rates[r];
                    devBits_ = bits[b];
                    devCh_ = chs[c];
                }
            }
        }
    }
    if (!hwo_) {
        post_log(hwnd_, "No usable waveOut device/format");
        return false;
    }
    /* Volume is always applied in software and rewritten into the queued
     * buffers on every change (regainQueued); the device volume (on Win9x
     * the system-wide Wave slider) is the user's. Builds before 2026-10
     * drove it from here and left it at 0 when the app quit while muted, so
     * once per run a Wave volume of exactly 0 is put back to full. */
    setGain(mute_ ? 0 : ((long)vol_ * 256 / 100));
    static bool waveChecked = false;
    if (!waveChecked) {
        waveChecked = true;
        UINT id = 0;
        WAVEOUTCAPSA caps;
        DWORD cur = 1;
        if (waveOutGetID(hwo_, &id) == MMSYSERR_NOERROR &&
            waveOutGetDevCapsA(id, &caps, sizeof(caps)) == MMSYSERR_NOERROR &&
            (caps.dwSupport & (WAVECAPS_VOLUME | WAVECAPS_LRVOLUME)) &&
            waveOutGetVolume(hwo_, &cur) == MMSYSERR_NOERROR && cur == 0)
            waveOutSetVolume(hwo_, 0xFFFFFFFF);
    }
    frames_ = (size_t)((long long)rate_ * s.bufferMs / 1000);
    if (frames_ < 256)
        frames_ = 256;
    if (frames_ > 16384)
        frames_ = 16384;
    int nc = s.bufCount;
    if (nc < 2)
        nc = 2;
    if (nc > 10)
        nc = 10;
    bufs_.resize(nc);
    for (size_t i = 0; i < bufs_.size(); i++) {
        bufs_[i].data.resize(frames_ * 2 * 2); // render as stereo16, convert on copy
        bufs_[i].raw.assign(frames_ * 2, 0);
        memset(&bufs_[i].hdr, 0, sizeof(WAVEHDR));
        bufs_[i].hdr.lpData = (LPSTR)bufs_[i].data.data();
        bufs_[i].hdr.dwBufferLength = (DWORD)(frames_ * (size_t)devCh_ * devBits_ / 8);
        bufs_[i].hdr.dwFlags = 0;
        if (waveOutPrepareHeader(hwo_, &bufs_[i].hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            /* e.g. no memory to page-lock on a small Win95 box; the caller
             * closes the device (stopPlayback unprepares what was prepared) */
            post_log(hwnd_, "Audio: cannot prepare %d buffers", (int)bufs_.size());
            return false;
        }
        bufs_[i].prepared = true;
    }
    totalRendered_ = 0;
    renderedBase_ = 0;
    /* fresh pacing measurement for this device open */
    spanEmaMs_ = 0;
    lastDoneTick_ = 0;
    doneCount_ = 0;
    spanLogged_ = false;
    clockMode_ = false;
    clockSamples_ = 0;
    clockLastTick_ = 0;
    clockSubmitted_ = 0;
    dryCount_ = 0;
    dryLogTick_ = 0;
    post_log(hwnd_, "Audio: %d Hz, %d bit, %d ch, %d x %d ms buffers%s", rate_, devBits_,
             devCh_, (int)bufs_.size(), (int)(frames_ * 1000 / rate_),
             eventDriven_ ? "" : " (polled)");
    return true;
}

void Engine::setGain(long g)
{
    gain_ = g < 0 ? 0 : (g > 256 ? 256 : g); /* <= 256: results always fit int16 */
    for (int b = 0; b < 256; b++) {
        gainHi_[b] = (int32_t)(int8_t)b * 256 * gain_ >> 8; /* = (int8_t)b * gain_ */
        gainLo_[b] = (int32_t)b * gain_ >> 8;
    }
}

/* apply gain + convert rendered stereo16 to the device format. Exact:
 * the tables give ((long)x * gain_) >> 8 for every int16 x, and with
 * gain_ <= 256 no result leaves the int16 range, so nothing is clamped. */
void Engine::convert(const int16_t *src, size_t n, BYTE *dst) const
{
    const int32_t *hi = gainHi_, *lo = gainLo_;
    if (devCh_ == 2 && devBits_ == 16) {
        if (gain_ == 256) { /* 100%: the render as is */
            memcpy(dst, src, n * 4);
            return;
        }
        if (gain_ == 0) { /* muted */
            memset(dst, 0, n * 4);
            return;
        }
        int16_t *o = (int16_t *)dst;
        for (size_t k = 0; k < n * 2; k++) {
            uint16_t u = (uint16_t)src[k];
            o[k] = (int16_t)(hi[u >> 8] + lo[u & 0xFF]);
        }
    } else if (devCh_ == 1 && devBits_ == 16) {
        int16_t *o = (int16_t *)dst;
        for (size_t k = 0; k < n; k++) {
            uint16_t u = (uint16_t)(int16_t)(((long)src[2 * k] + src[2 * k + 1]) / 2);
            o[k] = (int16_t)(hi[u >> 8] + lo[u & 0xFF]);
        }
    } else if (devCh_ == 2 && devBits_ == 8) {
        /* >> 8 floors (no bias around zero, unlike / 256) */
        for (size_t k = 0; k < n * 2; k++) {
            uint16_t u = (uint16_t)src[k];
            dst[k] = (BYTE)(((hi[u >> 8] + lo[u & 0xFF]) >> 8) + 128);
        }
    } else {
        for (size_t k = 0; k < n; k++) {
            uint16_t u = (uint16_t)(int16_t)(((long)src[2 * k] + src[2 * k + 1]) / 2);
            dst[k] = (BYTE)(((hi[u >> 8] + lo[u & 0xFF]) >> 8) + 128);
        }
    }
}

/* Volume/mute change: rewrite every queued buffer the driver hasn't
 * finished with the new gain, so the change is heard as soon as the
 * driver reaches audio it hasn't copied yet, instead of after the whole
 * queue (bufCount x bufferMs) has drained. A driver reading a buffer while
 * it is rewritten just gets some old and some new samples; the memory stays
 * ours until WHDR_DONE. */
void Engine::regainQueued()
{
    for (size_t i = 0; i < bufs_.size(); i++) {
        Buf &b = bufs_[i];
        if (b.queued && !(b.hdr.dwFlags & WHDR_DONE) && b.rendered > 0)
            convert(b.raw.data(), b.rendered, b.data.data());
    }
}

/* Called before each refill: if every buffer we queued has already been
 * played, the device ran out of audio and there was a gap in the sound
 * (the engine thread was held up longer than the whole queue lasts). Says
 * so in the log, so a dropout can be told apart from one that happens
 * further down the line (a VM's emulated sound card, Wine's audio). Not in
 * clock mode: those drivers flag buffers done long before playing them. */
bool Engine::checkDry()
{
    if (clockMode_ || eof_)
        return false;
    int queued = 0, done = 0;
    for (size_t i = 0; i < bufs_.size(); i++) {
        if (bufs_[i].queued) {
            queued++;
            if (bufs_[i].hdr.dwFlags & WHDR_DONE)
                done++;
        }
    }
    if (queued == 0 || done < queued)
        return false;
    dryCount_++;
    DWORD now = GetTickCount();
    if (dryLogTick_ == 0 || (DWORD)(now - dryLogTick_) >= 10000) {
        post_log(hwnd_, dryCount_ == 1 ? "Audio: playback ran dry (gap in the sound)"
                                       : "Audio: playback ran dry %d times (gaps in the sound)",
                 dryCount_);
        dryLogTick_ = now;
        dryCount_ = 0;
    }
    return true;
}

bool Engine::refill()
{
    if (checkDry())
        dryNow_ = true;
    for (size_t i = 0; i < bufs_.size(); i++) {
        Buf &b = bufs_[i];
        if (b.queued && (b.hdr.dwFlags & WHDR_DONE)) {
            b.queued = false;
            /* hardware-metered playback accounting: the driver just finished
             * exactly this buffer's rendered samples; re-anchor the
             * in-buffer interpolation here. Also measure how long a buffer
             * really takes (the device's true pace - may differ from the
             * nominal rate in VMs). */
            DWORD now = GetTickCount();
            if (lastDoneTick_ && !clockMode_) {
                DWORD span = now - lastDoneTick_;
                if (span > 0 && span < 30000) {
                    if (spanEmaMs_ <= 0)
                        spanEmaMs_ = span;
                    else
                        spanEmaMs_ = spanEmaMs_ * 0.7 + (double)span * 0.3;
                    double nom = frames_ * 1000.0 / (rate_ ? rate_ : 44100);
                    if (doneCount_ >= 4 && spanEmaMs_ < nom * 0.35) {
                        /* completions arrive far faster than the buffers
                         * can possibly play: this driver flags done early,
                         * so completion pacing would run away - switch to
                         * the nominal-rate clock instead */
                        clockMode_ = true;
                        /* stay continuous: this buffer's samples count too */
                        clockSamples_ = compSamples_ + (long long)b.rendered;
                        clockLastTick_ = now;
                        clockSubmitted_ = clockSamples_;
                        post_log(hwnd_, "Audio driver releases buffers early - "
                                        "tracker switched to clock timing");
                    }
                }
            }
            lastDoneTick_ = now;
            if (!clockMode_) {
                compSamples_ += (long long)b.rendered;
                doneCount_++;
                if (doneCount_ == 16 && !spanLogged_ && spanEmaMs_ > 0 && rate_ > 0) {
                    spanLogged_ = true;
                    post_log(hwnd_, "Audio: buffer span ~%.0f ms (nominal %.0f ms)",
                             spanEmaMs_, frames_ * 1000.0 / rate_);
                }
            }
            bufAnchorTick_ = now;
        }
        if (b.queued || eof_)
            continue;
        if (clockMode_ &&
            clockSubmitted_ - clockSamples_ > (long long)frames_ * 2)
            continue; /* clock mode: stay within ~2 buffers of the clock */
        size_t outBytes = frames_ * (size_t)devCh_ * devBits_ / 8;
        BYTE *out = b.data.data();
        memset(out, devBits_ == 8 ? 0x80 : 0, outBytes);
        /* render in ~20 ms chunks instead of one big read: after every
         * chunk we record the exact (samples, order, row) the module
         * reached, giving the tracker view row-level anchors so it can
         * follow playback without buffer-sized quantization */
        size_t chunk = (size_t)rate_ / 50;
        if (chunk < 64)
            chunk = 64;
        size_t done = 0;
        bool bufEof = false;
        while (done < frames_) {
            size_t want = frames_ - done < chunk ? frames_ - done : chunk;
            int16_t *src = b.raw.data() + done * 2;
            size_t n = mod_.readInt16(rate_, want, src);
            if (n == 0) {
                bufEof = true;
                break;
            }
            totalRendered_ += (long long)n; /* render position in frames */
            RowHist e;
            e.samples = totalRendered_;
            e.order = mod_.order();
            e.row = mod_.row();
            if (rowHist_.empty() || rowHist_.back().samples != e.samples)
                rowHist_.push_back(e);
            convert(src, n, out + done * (size_t)devCh_ * devBits_ / 8);
            done += n;
        }
        if (done == 0) {
            eof_ = true;
            return true;
        }
        b.hdr.dwFlags &= ~WHDR_DONE;
        b.rendered = done;
        if (waveOutWrite(hwo_, &b.hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
            return false; /* device gone/unprepared: rendering on would race
                           * through the song in silence */
        b.queued = true;
        clockSubmitted_ += (long long)done;
        playedSec_ += (double)done / rate_;
        if (bufEof) {
            eof_ = true; /* last partial buffer padded with silence */
            return true;
        }
    }
    return true;
}
