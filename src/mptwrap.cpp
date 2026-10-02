#include "mptwrap.h"

bool MptModule::loadFile(const std::string &path, int subsong, std::string *err,
                         bool *transient)
{
    close();
    if (transient)
        *transient = false;
    std::vector<char> data;
    if (!m95_read_file(path, data)) {
        if (err)
            *err = "cannot read file";
        if (transient) /* missing, locked, I/O error (or > 24 MB) */
            *transient = true;
        return false;
    }
    int error = 0;
    const char *errmsg = nullptr;
    /* no dither on the 27-bit -> 16-bit output step: it only shapes the
     * noise floor (inaudible on period sound cards) and costs ~5% of the
     * render time, a PRNG call per sample (perf sweep, docs/PERF-PLAN.md) */
    static const openmpt_module_initial_ctl ctls[] = { { "dither", "0" }, { nullptr, nullptr } };
    m_ = openmpt_module_create_from_memory2(data.data(), data.size(), nullptr, nullptr,
                                            nullptr, nullptr, &error, &errmsg, ctls);
    if (!m_) {
        if (transient && error == OPENMPT_ERROR_OUT_OF_MEMORY)
            *transient = true;
        if (err)
            *err = errmsg ? errmsg : "unsupported or broken module";
        openmpt_free_string(errmsg); /* the caller owns it (NULL is fine) */
        return false;
    }
    if (subsong > 0)
        openmpt_module_select_subsong(m_, subsong - 1);
    openmpt_module_set_repeat_count(m_, 0);
    return true;
}

void MptModule::close()
{
    if (m_) {
        openmpt_module_destroy(m_);
        m_ = nullptr;
    }
}

size_t MptModule::readInt16(int rate, size_t frames, int16_t *out)
{
    return m_ ? openmpt_module_read_interleaved_stereo(m_, rate, frames, out) : 0;
}

double MptModule::duration() const { return m_ ? openmpt_module_get_duration_seconds(m_) : -1.0; }
double MptModule::position() const { return m_ ? openmpt_module_get_position_seconds(m_) : 0.0; }
double MptModule::seek(double s) { return m_ ? openmpt_module_set_position_seconds(m_, s) : 0.0; }
int MptModule::order() const { return m_ ? openmpt_module_get_current_order(m_) : 0; }
int MptModule::pattern() const { return m_ ? openmpt_module_get_current_pattern(m_) : 0; }
int MptModule::row() const { return m_ ? openmpt_module_get_current_row(m_) : 0; }
int MptModule::speed() const { return m_ ? openmpt_module_get_current_speed(m_) : 0; }
int MptModule::bpm() const
{
    return m_ ? (int)(openmpt_module_get_current_estimated_bpm(m_) + 0.5) : 0;
}
int MptModule::channels() const { return m_ ? openmpt_module_get_num_channels(m_) : 0; }
int MptModule::numOrders() const { return m_ ? openmpt_module_get_num_orders(m_) : 0; }
int MptModule::orderPattern(int order) const
{
    return m_ ? openmpt_module_get_order_pattern(m_, order) : -1;
}
int MptModule::patternRows(int pattern) const
{
    return m_ ? openmpt_module_get_pattern_num_rows(m_, pattern) : 0;
}
std::string MptModule::cellText(int pattern, int row, int channel, int cmd) const
{
    if (!m_)
        return "";
    const char *s =
        openmpt_module_format_pattern_row_channel_command(m_, pattern, row, channel, cmd);
    if (!s)
        return "";
    std::string r = s;
    openmpt_free_string(s);
    return r;
}
int MptModule::subsongs() const { return m_ ? openmpt_module_get_num_subsongs(m_) : 1; }
bool MptModule::selectSubsong(int n) { return m_ ? openmpt_module_select_subsong(m_, n) != 0 : false; }
void MptModule::setRepeat(int n)
{
    if (m_)
        openmpt_module_set_repeat_count(m_, n);
}
void MptModule::setCtl(const char *key, const char *value)
{
    if (m_)
        openmpt_module_ctl_set_text(m_, key, value);
}
void MptModule::setRenderParam(int param, int value)
{
    if (m_)
        openmpt_module_set_render_param(m_, param, value);
}

std::string MptModule::meta(const char *key) const
{
    if (!m_)
        return "";
    const char *s = openmpt_module_get_metadata(m_, key);
    if (!s)
        return "";
    /* libopenmpt returns UTF-8; the ANSI controls show the ANSI codepage */
    std::string r = m95_utf8_to_ansi(s);
    openmpt_free_string(s);
    return r;
}

std::vector<std::string> mpt_supported_extensions()
{
    std::vector<std::string> out;
    const char *s = openmpt_get_supported_extensions();
    if (!s)
        return out;
    std::string all = s;
    size_t pos = 0;
    /* libopenmpt returns a semicolon-separated list; tolerate commas/spaces too */
    while (pos < all.size()) {
        size_t c = all.find_first_of(",; \t", pos);
        if (c == std::string::npos)
            c = all.size();
        std::string e = all.substr(pos, c - pos);
        if (!e.empty())
            out.push_back(m95_lower(e));
        pos = c + 1;
    }
    return out;
}

std::string mpt_version_string()
{
    uint32_t v = openmpt_get_library_version();
    char buf[64];
    /* (major << 24) | (minor << 16) | patch */
    sprintf(buf, "%u.%u.%u", (v >> 24) & 0xff, (v >> 16) & 0xff, v & 0xffff);
    return buf;
}
