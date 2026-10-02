#pragma once
#include "mptwrap.h"
#include <map>

/* one fetched tracker-view window; ownership passes with the WM_APP_ROWS
 * message to the UI thread */
struct RowsMsg
{
    int order = 0, row = 0, half = 0;
    int off = 0, nch = 0, total = 0; /* channel paging window */
    std::vector<std::string> rows;
};

struct AudioSettings
{
    int rate = 44100;
    int interpolation = 2;
    int bufferMs = 220;
    int bufCount = 6;
};

struct EngineSnap
{
    bool loaded = false;
    bool playing = false;
    bool paused = false;
    double pos = 0, dur = 0;
    int order = 0, pat = 0, row = 0, speed = 0, bpm = 0, channels = 0;
    int rate = 0;
    double heard = 0; /* audible position in seconds (vs pos = render pos) */
    int sub = 0;      /* subsong being played, 1-based (0: none) */
    unsigned gen = 0; /* load/stop generation this snapshot belongs to */
    int bufMs = 0;    /* length of one output buffer in use */
    bool trkValid = false;   /* tracker-display row (= heard position) */
    int trkOrder = 0, trkPat = 0, trkRow = 0;
    /* perf-note counters (opt run 7): session-cumulative */
    double played = 0; /* seconds of audio rendered since device open */
    long posts = 0;    /* tracker row windows posted */
};

/* waveOut playback engine. All module/device access lives on one thread. */
class Engine
{
public:
    ~Engine() { shutdown(); }
    void init(HWND hwnd);
    /* false if the engine thread didn't stop in time: it then still owns
     * its state, so nothing may be torn down (the caller must end the
     * process without running static destructors) */
    bool shutdown();

    /* start a song; with startPaused it sits paused at startPos without
     * ever reaching the speakers (session resume). Every load and stop
     * starts a new generation: WM_APP_ENDED carries it in wParam (and a
     * reason in lParam: 0 ended, 1 load failed, 2 audio device/output
     * failed), so a message from an earlier song can be told apart. */
    void load(const std::string &path, int subsong, const AudioSettings &s,
              double startPos = 0, bool startPaused = false);
    void stop();
    unsigned generation() const { return gen_; }
    void seekTo(double seconds);
    void pauseToggle();
    void setVolume(int vol0to100, bool mute);
    /* tracker view: enable/disable and set the visible window
     * (2*half+1 rows centered on the current row, channels off..off+nch-1).
     * While on, the engine posts WM_APP_ROWS itself whenever the row
     * changes - no polling needed on the UI side. */
    void fetchRows(bool on, int half, int nch, int off);
    /* extra display delay (ms) for the tracker view, to compensate output
     * latency beyond this app's buffers; applies immediately */
    void setTrkDelay(int ms);
    /* synchronous copy of the most recently posted tracker window (or NULL).
     * Lets the UI refresh the pattern view without waiting on posted
     * messages, which Win95 starves while the list is being scrolled. */
    RowsMsg *grabLatestRows();
    /* minimum ms between tracker-view posts (0 = post every row change);
     * also meant to match the UI's info-refresh period */
    void setRefresh(int ms);
    EngineSnap snap();

private:
    struct Cmd
    {
        enum { LOAD, STOP, SEEK, PAUSE, VOL, FETCH, DELAY, REFRESH, QUIT } t;
        double d = 0;
        int i = 0, i2 = 0, i3 = 0;
        bool b = false;
        unsigned gen = 0;
        std::string path;
        AudioSettings s;
    };
    void push(const Cmd &c);
    static DWORD WINAPI threadMain(LPVOID p);
    void run();
    std::vector<std::string> &ensurePattern(int pat, int off, int nch);
    std::string rowLine(int pat, int row, int off, int nch);
    bool stepNextRow(int &o, int &r);
    bool stepPrevRow(int &o, int &r);
    RowsMsg *buildRowsMsg(int order, int row);
    void maybePostRows();

    void stopPlayback();
    bool openDevice(const AudioSettings &s);
    bool refill(); /* false: waveOutWrite failed */
    void seekInternal(double seconds);
    void failPlayback(int reason, const char *why);
    double paceMs() const;
    void convert(const int16_t *src, size_t frames, BYTE *dst) const;
    void setGain(long g);
    void regainQueued();
    bool checkDry();

    HWND hwnd_ = NULL;
    HANDLE cmdEv_ = NULL;
    HANDLE audioEv_ = NULL;
    bool eventDriven_ = false;
    CRITICAL_SECTION cs_;
    std::vector<Cmd> cmds_;
    HANDLE thread_ = NULL;
    bool quit_ = false;    /* engine thread: a QUIT was received */
    unsigned gen_ = 0;     /* UI thread: generation of the latest load/stop */
    unsigned curGen_ = 0;  /* engine thread: generation being played */
    bool inited_ = false;

    MptModule mod_;
    struct PatCache
    {
        int nch = 0;
        std::vector<std::string> rows;
    };
    std::map<long long, PatCache> patCache_; /* tracker view text, engine thread only */
    /* tracker view params, engine thread only */
    bool trkOn_ = false;
    int trkHalf_ = 0, trkNch_ = 0, trkOff_ = 0;
    int lastTrkOrder_ = -1, lastTrkRow_ = -1;
    /* row tracking: history of (frames rendered so far, order, row) observed
     * on the engine thread; the row to show is the one at the position
     * actually heard, derived from the playback clock (see below) */
    struct RowHist
    {
        long long samples;
        int order, row;
    };
    std::vector<RowHist> rowHist_;
    long long totalRendered_ = 0; /* frames rendered since device open */
    long posts_ = 0;              /* row windows posted (perf note) */
    double playedSec_ = 0;        /* audio rendered this session (perf note) */
    long long renderedBase_ = 0;  /* totalRendered_ at last (re)start */
    double heardBase_ = 0;        /* song position (s) at last (re)start */
    /* audible position tracking, hardware-metered: every wave buffer the
     * driver marks WHDR_DONE held exactly its rendered sample count, so
     * compSamples_ is the number of samples the device has truly played.
     * Only the single in-progress buffer is interpolated by wall time, and
     * that interpolation is re-anchored on every completion, so no error
     * can ever accumulate beyond one buffer. No units to misread, no rate
     * assumption, no clock that can drift. */
    long long compSamples_ = 0;  /* samples played, summed per completion */
    long long interpFrac_ = 0;   /* frozen in-buffer fraction while paused */
    DWORD bufAnchorTick_ = 0;    /* when the in-progress buffer started */
    int trkDelayMs_ = 0;         /* user display delay for the tracker view */
    int refreshMs_ = 0;          /* min ms between tracker posts (0 = none) */
    DWORD lastPostTick_ = 0;
    RowsMsg *latestRows_ = NULL; /* copy of last post, guarded by cs_ */
    /* v9 pacing measurement: exponential average of buffer-completion spans.
     * The in-buffer fraction advances at the MEASURED device pace instead of
     * the nominal rate, so the displayed tempo tracks the audible sound even
     * if the device clock differs from the wall clock (common in VMs). */
    double spanEmaMs_ = 0;
    DWORD lastDoneTick_ = 0;
    int doneCount_ = 0;
    bool spanLogged_ = false;
    /* clock fallback for drivers that mark buffers done before playing them
     * (then completion pacing no longer follows playback): the heard
     * position comes from a nominal-rate wall clock and buffer submission
     * is throttled to nominal consumption, so nothing can run away */
    bool clockMode_ = false;
    /* playback ran dry (every queued buffer finished before the refill):
     * an audible gap caused on our side; logged, rate-limited */
    int dryCount_ = 0;
    bool dryNow_ = false; /* the last refill found the queue empty */
    DWORD dryLogTick_ = 0;
    long long clockSamples_ = 0;
    DWORD clockLastTick_ = 0;
    long long clockSubmitted_ = 0;
    /* anchor observed right after load/seek: the exact (order, row) at the
     * moment playback (re)starts, before the first refill runs ahead */
    bool seedValid_ = false;
    long long seedSamples_ = 0;
    int seedOrder_ = 0, seedRow_ = 0;
    HWAVEOUT hwo_ = NULL;
    struct Buf
    {
        std::vector<BYTE> data;    /* device-format samples, gain applied */
        std::vector<int16_t> raw;  /* the same audio as rendered (stereo16, no
                                    * gain), so a volume change can rewrite
                                    * a queued buffer before it plays */
        WAVEHDR hdr;
        bool queued = false;
        bool prepared = false;
        size_t rendered = 0; /* frames actually rendered into this buffer */
    };
    std::vector<Buf> bufs_;
    bool playing_ = false, paused_ = false, eof_ = false, endedSent_ = true;
    int rate_ = 0, devBits_ = 16, devCh_ = 2;
    size_t frames_ = 0;
    long gain_ = 204; // 0..256, applied in software (see regainQueued)
    /* (x * gain_) >> 8 for an int16 x = hi*256 + lo is exactly
     * gainHi_[hi] + gainLo_[lo]: two loads and an add instead of an IMUL
     * (13-42 cycles on a 486) per sample; 2 KB, rebuilt by setGain */
    int32_t gainHi_[256]; /* indexed by the high byte (as signed) */
    int32_t gainLo_[256]; /* indexed by the low byte (unsigned) */
    int vol_ = 80;
    bool mute_ = false;
    EngineSnap snap_;
};
