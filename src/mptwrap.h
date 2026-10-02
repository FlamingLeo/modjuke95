#pragma once
#include "common.h"
#include "libopenmpt/libopenmpt.h"

/* Thin RAII wrapper over the libopenmpt C API. */
class MptModule
{
public:
    MptModule() = default;
    ~MptModule() { close(); }
    MptModule(MptModule &&o) noexcept : m_(o.m_) { o.m_ = nullptr; }
    MptModule &operator=(MptModule &&o) noexcept
    {
        if (this != &o) {
            close();
            m_ = o.m_;
            o.m_ = nullptr;
        }
        return *this;
    }
    MptModule(const MptModule &) = delete;
    MptModule &operator=(const MptModule &) = delete;

    /* *transient (optional): the failure says nothing about the file
     * (read error, out of memory) - don't remember it as broken */
    bool loadFile(const std::string &path, int subsong, std::string *err,
                  bool *transient = nullptr);
    void close();
    bool isOpen() const { return m_ != nullptr; }

    size_t readInt16(int rate, size_t frames, int16_t *out);
    double duration() const;
    double position() const;
    double seek(double seconds);
    int order() const;
    int pattern() const;
    int row() const;
    int speed() const;
    int bpm() const;
    int channels() const;
    int subsongs() const;
    int subsong() const; /* selected subsong, 1-based (0: all or none) */
    /* pattern data for the tracker view */
    int numOrders() const;
    int orderPattern(int order) const;
    int patternRows(int pattern) const;
    std::string cellText(int pattern, int row, int channel, int cmd) const;
    bool selectSubsong(int n);
    void setRepeat(int n);
    void setCtl(const char *key, const char *value);
    /* render params (see OPENMPT_MODULE_RENDER_* in libopenmpt.h) */
    void setRenderParam(int param, int value);
    std::string meta(const char *key) const;

private:
    openmpt_module *m_ = nullptr;
};

std::vector<std::string> mpt_supported_extensions();
std::string mpt_version_string();
