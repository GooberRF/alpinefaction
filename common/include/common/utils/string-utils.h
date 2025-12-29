#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <iterator>
#include <cctype>

#ifdef __GNUC__
#define PRINTF_FMT_ATTRIBUTE(fmt_idx, va_idx) __attribute__ ((format (printf, fmt_idx, va_idx)))
#else
#define PRINTF_FMT_ATTRIBUTE(fmt_idx, va_idx)
#endif

inline std::string_view ltrim(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    return s;
}

inline std::string_view rtrim(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

inline std::string_view trim(std::string_view s)
{
    return rtrim(ltrim(s));
}

inline std::pair<std::string_view, std::string_view> split_once_whitespace(std::string_view s)
{
    s = trim(s);
    const auto pos = s.find_first_of(" \t");
    if (pos == std::string_view::npos)
        return {s, {}};
    return {trim(s.substr(0, pos)), trim(s.substr(pos + 1))};
}

inline std::vector<std::string_view> string_split(std::string_view str, char delim = ' ')
{
    std::vector<std::string_view> output;
    size_t first = 0;

    while (first < str.size()) {
        const auto second = str.find_first_of(delim, first);

        if (first != second)
            output.emplace_back(str.substr(first, second - first));

        if (second == std::string_view::npos)
            break;

        first = second + 1;
    }

    return output;
}

inline std::string string_to_lower(std::string_view str)
{
    std::string output;
    output.reserve(str.size());
    std::transform(str.begin(), str.end(), std::back_inserter(output), [](unsigned char ch) {
        return std::tolower(ch);
    });
    return output;
}

inline std::string string_to_upper(std::string_view str)
{
    std::string output;
    output.reserve(str.size());
    std::transform(str.begin(), str.end(), std::back_inserter(output), [](unsigned char ch) {
        return std::toupper(ch);
    });
    return output;
}

inline bool string_iequals(std::string_view left, std::string_view right)
{
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(), [](unsigned char a, unsigned char b) {
        return std::tolower(a) == std::tolower(b);
    });
}

inline bool string_starts_with(std::string_view str, std::string_view prefix)
{
    return str.starts_with(prefix);
}

inline bool string_istarts_with(std::string_view str, std::string_view prefix)
{
    return string_iequals(str.substr(0, prefix.size()), prefix);
}

inline bool string_ends_with(std::string_view str, std::string_view suffix)
{
    return str.ends_with(suffix);
}

inline bool string_iends_with(std::string_view str, std::string_view suffix)
{
    return str.size() >= suffix.size() && string_iequals(str.substr(str.size() - suffix.size()), suffix);
}

inline bool string_contains(std::string_view str, char ch)
{
    return str.find(ch) != std::string_view::npos;
}

inline bool string_contains(std::string_view str, std::string_view infix)
{
    return str.find(infix) != std::string_view::npos;
}

inline bool string_icontains(std::string_view str, std::string_view infix)
{
    std::string_view::iterator it = std::search(str.begin(), str.end(),
        infix.begin(), infix.end(),  [](unsigned char a, unsigned char b) {
        return std::tolower(a) == std::tolower(b);
    });
    return it != str.end();
}

inline std::string string_replace(const std::string_view& str, const std::string_view& search, const std::string_view& replacement)
{
    std::size_t pos = 0;
    std::string result{str};
    while (true) {
        pos = result.find(search, pos);
        if (pos == std::string::npos) {
            break;
        }
        result.replace(pos, search.size(), replacement);
        pos += replacement.size();
    }
    return result;
}

inline std::string string_add_suffix_before_extension(std::string_view filename, std::string_view suffix)
{
    if (suffix.empty())
        return std::string{filename};

    const size_t dot = filename.rfind('.');
    if (dot == std::string_view::npos) {
        std::string out;
        out.reserve(filename.size() + suffix.size());
        out.append(filename);
        out.append(suffix);
        return out;
    }

    std::string out;
    out.reserve(filename.size() + suffix.size());
    out.append(filename.substr(0, dot));
    out.append(suffix);
    out.append(filename.substr(dot));
    return out;
}

inline std::string string_remove_suffix_before_extension(std::string_view filename, std::string_view suffix, bool case_sensitive = false)
{
    if (suffix.empty())
        return std::string{filename};

    const size_t dot = filename.rfind('.');
    const std::string_view stem = (dot == std::string_view::npos) ? filename : filename.substr(0, dot);
    const std::string_view ext  = (dot == std::string_view::npos) ? std::string_view{} : filename.substr(dot);

    if (stem.size() < suffix.size())
        return std::string{filename};

    const std::string_view tail = stem.substr(stem.size() - suffix.size());

    const bool match = case_sensitive ? (tail == suffix) : string_iequals(tail, suffix);
    if (!match)
        return std::string{filename};

    std::string out;
    out.reserve(filename.size());
    out.append(stem.substr(0, stem.size() - suffix.size()));
    out.append(ext);
    return out;
}

inline std::string string_remove_any_suffix_before_extension(
    std::string_view filename,
    std::initializer_list<std::string_view> suffixes,
    bool case_sensitive = false)
{
    for (const auto suffix : suffixes) {
        std::string candidate = string_remove_suffix_before_extension(filename, suffix, case_sensitive);
        if (candidate != filename) {
            return candidate;
        }
    }
    return std::string{filename};
}

inline bool string_has_suffix_before_extension(
    std::string_view filename,
    std::string_view suffix,
    bool case_sensitive = false)
{
    if (suffix.empty())
        return true;

    const size_t dot = filename.rfind('.');
    const std::string_view stem = (dot == std::string_view::npos) ? filename : filename.substr(0, dot);

    if (stem.size() < suffix.size())
        return false;

    const std::string_view tail = stem.substr(stem.size() - suffix.size());
    return case_sensitive ? (tail == suffix) : string_iequals(tail, suffix);
}

struct StringMatcher
{
private:
    std::string m_exact, m_prefix, m_infix, m_suffix;
    bool m_case_sensitive;

public:
    StringMatcher(bool case_sensitive = false) : m_case_sensitive(case_sensitive) {}

    StringMatcher& exact(const std::string& exact)
    {
        this->m_exact = exact;
        return *this;
    }

    StringMatcher& prefix(const std::string& prefix)
    {
        this->m_prefix = prefix;
        return *this;
    }

    StringMatcher& infix(const std::string& infix)
    {
        this->m_infix = infix;
        return *this;
    }

    StringMatcher& suffix(const std::string& suffix)
    {
        this->m_suffix = suffix;
        return *this;
    }

    bool operator()(std::string_view input) const
    {
        if (m_case_sensitive) {
            if (!m_exact.empty() && input != m_exact)
                return false;
            if (!m_prefix.empty() && !string_starts_with(input, m_prefix))
                return false;
            if (!m_infix.empty() && !string_contains(input, m_infix))
                return false;
            if (!m_suffix.empty() && !string_ends_with(input, m_suffix))
                return false;
        }
        else {
            if (!m_exact.empty() && !string_iequals(input, m_exact))
                return false;
            if (!m_prefix.empty() && !string_istarts_with(input, m_prefix))
                return false;
            if (!m_infix.empty() && !string_icontains(input, m_infix))
                return false;
            if (!m_suffix.empty() && !string_iends_with(input, m_suffix))
                return false;
        }
        return true;
    }
};

inline std::string_view get_filename_without_ext(std::string_view filename)
{
    auto dot_pos = filename.rfind('.');
    if (dot_pos == std::string_view::npos) {
        return filename;
    }
    return filename.substr(0, dot_pos);
}

inline std::string_view get_ext_from_filename(std::string_view filename)
{
    auto dot_pos = filename.rfind('.');
    if (dot_pos == std::string_view::npos) {
        return "";
    }
    return filename.substr(dot_pos + 1);
}

