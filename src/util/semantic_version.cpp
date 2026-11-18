#include <util/semantic_version.hpp>

#include <util/text_parse.hpp>

SemanticVersion::SemanticVersion(const std::string &versionString)
{
    unsigned long major, minor, patch;
    if (TextParse::ParseVersion(versionString, major, minor, patch, m_extra))
    {
        m_major = static_cast<uint16_t>(major);
        m_minor = static_cast<uint16_t>(minor);
        m_patch = static_cast<uint16_t>(patch);
    }
    else {
        m_major = 0;
        m_minor = 0;
        m_patch = 0;
    }
}

std::string SemanticVersion::String()
{
    if (m_extra.empty())
    {
        return std::string("v" + std::to_string(m_major) + "." + std::to_string(m_minor) + "." + std::to_string(m_patch) + "\0");
    }

    return std::string("v" + std::to_string(m_major) + "." + std::to_string(m_minor) + "." + std::to_string(m_patch) + "-" + m_extra + "\0");
}

const char *SemanticVersion::CString()
{
    // Kept in the object: the pointer stays valid until the next call or until the object is destroyed.
    m_string = String();
    return m_string.c_str();
}

bool operator==(const SemanticVersion &sv1, const SemanticVersion &sv2)
{
    // Memory address is the same.
    if (&sv1 == &sv2)
    {
        return true;
    }

    if (sv1.m_major == sv2.m_major && sv1.m_minor == sv2.m_minor && sv1.m_patch == sv2.m_patch)
    {
        return true;
    }

    return false;
}

bool operator!=(const SemanticVersion &sm1, const SemanticVersion &sm2)
{
    return !(sm1 == sm2);
}

bool operator>(const SemanticVersion &sv1, const SemanticVersion &sv2)
{
    if (sv1 == sv2)
    {
        return false;
    }

    if (sv1.m_major > sv2.m_major)
    {
        return true;
    }
    else if (sv1.m_major < sv2.m_major)
    {
        return false;
    }

    if (sv1.m_minor > sv2.m_minor)
    {
        return true;
    }
    else if (sv1.m_minor < sv2.m_minor)
    {
        return false;
    }

    if (sv1.m_patch > sv2.m_patch)
    {
        return true;
    }

    return false;
}

bool operator<(const SemanticVersion &sv1, const SemanticVersion &sv2)
{
    if (sv1 == sv2)
    {
        return false;
    }

    if (sv1.m_major < sv2.m_major)
    {
        return true;
    }
    else if (sv1.m_major > sv2.m_major)
    {
        return false;
    }

    if (sv1.m_minor < sv2.m_minor)
    {
        return true;
    }
    else if (sv1.m_minor > sv2.m_minor)
    {
        return false;
    }

    if (sv1.m_patch < sv2.m_patch)
    {
        return true;
    }

    return false;
}

bool operator>=(const SemanticVersion &sv1, const SemanticVersion &sv2)
{
    if (sv1 == sv2 || sv1 > sv2)
    {
        return true;
    }

    return false;
}

bool operator<=(const SemanticVersion &sv1, const SemanticVersion &sv2)
{
    if (sv1 == sv2 || sv1 < sv2)
    {
        return true;
    }

    return false;
}
