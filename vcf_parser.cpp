// vcf_parser.cpp — tolerant vCard 2.1/3.0 parser with QP + charset
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <stdint.h>

#include "vcf_parser.hpp"

// ------ trim/split ------
static std::string trimA(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= 0x20) ++a;
    while (b > a && (unsigned char)s[b - 1] <= 0x20) --b;
    return s.substr(a, b - a);
}
static std::wstring trimW(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}
static void splitA(const std::string& s, char ch, std::vector<std::string>& out) {
    out.clear();
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find(ch, i);
        if (j == std::string::npos) { out.push_back(s.substr(i)); break; }
        out.push_back(s.substr(i, j - i));
        i = j + 1;
    }
}

// ------ codepage/wide ------
static std::wstring ToWideFromCP(const std::string& bytes, UINT cp) {
    if (bytes.empty()) return L"";
    int wlen = MultiByteToWideChar(cp, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    if (wlen <= 0) return L"";
    std::wstring w(wlen, L'\0');
    MultiByteToWideChar(cp, 0, bytes.data(), (int)bytes.size(), w.data(), wlen);
    return w;
}
static UINT DetectCodepage(const std::wstring& charset) {
    std::wstring cs = charset;
    for (auto& ch : cs) ch = towupper(ch);
    if (cs.find(L"UTF-8") != std::wstring::npos || cs.find(L"UTF8") != std::wstring::npos) return CP_UTF8;
    if (cs.find(L"1251") != std::wstring::npos || cs.find(L"WINDOWS-1251") != std::wstring::npos) return 1251;
    if (cs.find(L"KOI8") != std::wstring::npos) return 20866;
    if (cs.find(L"866") != std::wstring::npos)  return 866;
    return CP_ACP;
}

// ------ quoted-printable ------
static std::string DecodeQP(const std::string& in) {
    std::string out; out.reserve(in.size());
    const unsigned char* s = (const unsigned char*)in.data();
    size_t n = in.size();
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = s[i];
        if (c == '=') {
            // soft breaks
            if (i + 1 < n && s[i + 1] == '\r' && i + 2 < n && s[i + 2] == '\n') { i += 2; continue; }
            if (i + 1 < n && (s[i + 1] == '\n' || s[i + 1] == '\r')) { ++i; continue; }
            auto hexval = [](unsigned char ch)->int {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                return -1;
                };
            if (i + 2 < n) {
                int h1 = hexval(s[i + 1]), h2 = hexval(s[i + 2]);
                if (h1 >= 0 && h2 >= 0) { out.push_back((char)((h1 << 4) | h2)); i += 2; continue; }
            }
            continue;
        }
        out.push_back((char)c);
    }
    return out;
}

// ------ base64 minimal ------
static std::vector<uint8_t> DecodeB64(const std::string& inA) {
    static const int T[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,52,53,54,55,56,57,58,59,60,61,-1,-1,-1, 0,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,
        -1,-1,-1,-1,-1,-1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    std::string in;
    in.reserve(inA.size());
    for (unsigned char c : inA) {
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        in.push_back((char)c);
    }
    std::vector<uint8_t> out;
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        int d = T[c];
        if (d == -1) continue;
        val = (val << 6) + d; valb += 6;
        if (valb >= 0) { out.push_back((uint8_t)((val >> valb) & 0xFF)); valb -= 8; }
    }
    return out;
}

// ------ unfolding ------
static std::vector<std::string> Unfold(const std::wstring& w) {
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0'); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), len, nullptr, nullptr);

    // normalize to \n
    std::string n; n.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\r') { if (i + 1 < s.size() && s[i + 1] == '\n') { ++i; } n.push_back('\n'); }
        else n.push_back(c);
    }

    std::vector<std::string> raw; splitA(n, '\n', raw);
    std::vector<std::string> out;
    for (auto& line : raw) {
        if (!out.empty() && !line.empty() && (line[0] == ' ' || line[0] == '\t')) {
            out.back() += line.substr(1);
        }
        else {
            out.push_back(line);
        }
    }
    return out;
}

// ------ params ------
struct PropParams {
    std::wstring charset;
    std::wstring encoding; // QUOTED-PRINTABLE / B / BASE64
    std::vector<std::wstring> types;
};

static PropParams ParseParams(const std::string& header) {
    PropParams pp;
    std::string nameAndParams = header;
    size_t semi = nameAndParams.find(':');
    if (semi != std::string::npos) nameAndParams = nameAndParams.substr(0, semi);

    size_t pos = nameAndParams.find(';');
    if (pos == std::string::npos) return pp;
    std::string params = nameAndParams.substr(pos + 1);

    std::vector<std::string> parts; splitA(params, ';', parts);
    for (auto& p : parts) {
        std::string kv = trimA(p);
        size_t eq = kv.find('=');
        std::string key = eq == std::string::npos ? kv : kv.substr(0, eq);
        std::string val = eq == std::string::npos ? "" : kv.substr(eq + 1);

        std::wstring wkey; wkey.resize(MultiByteToWideChar(CP_UTF8, 0, key.c_str(), (int)key.size(), nullptr, 0));
        MultiByteToWideChar(CP_UTF8, 0, key.c_str(), (int)key.size(), wkey.data(), (int)wkey.size());
        for (auto& ch : wkey) ch = towupper(ch);

        std::wstring wval; wval.resize(MultiByteToWideChar(CP_UTF8, 0, val.c_str(), (int)val.size(), nullptr, 0));
        MultiByteToWideChar(CP_UTF8, 0, val.c_str(), (int)val.size(), wval.data(), (int)wval.size());

        if (wkey.find(L"CHARSET") != std::wstring::npos) pp.charset = wval;
        else if (wkey.find(L"ENCODING") != std::wstring::npos) {
            for (auto& ch : wval) ch = towupper(ch);
            pp.encoding = wval;
        }
        else if (wkey.find(L"TYPE") != std::wstring::npos) {
            std::vector<std::string> tp; splitA(val, ',', tp);
            for (auto& t : tp) {
                std::wstring wt; wt.resize(MultiByteToWideChar(CP_UTF8, 0, t.c_str(), (int)t.size(), nullptr, 0));
                MultiByteToWideChar(CP_UTF8, 0, t.c_str(), (int)t.size(), wt.data(), (int)wt.size());
                pp.types.push_back(wt);
            }
        }
    }
    return pp;
}

static std::wstring DecodeFieldText(const std::string& rawValue, const PropParams& p) {
    std::wstring out;
    if (!p.encoding.empty()) {
        std::wstring enc = p.encoding; for (auto& c : enc) c = towupper(c);
        if (enc.find(L"QUOTED-PRINTABLE") != std::wstring::npos) {
            std::string bytes = DecodeQP(rawValue);
            UINT cp = DetectCodepage(p.charset);
            out = ToWideFromCP(bytes, cp);
            if (out.empty() && cp != CP_UTF8) out = ToWideFromCP(bytes, CP_UTF8);
            if (out.empty()) out = ToWideFromCP(bytes, CP_ACP);
            return trimW(out);
        }
        if (enc == L"B" || enc == L"BASE64") {
            std::vector<uint8_t> b = DecodeB64(rawValue);
            std::string bytes((const char*)b.data(), b.size());
            out = ToWideFromCP(bytes, CP_UTF8);
            if (out.empty()) out = ToWideFromCP(bytes, CP_ACP);
            return trimW(out);
        }
    }
    // plain text
    out = ToWideFromCP(rawValue, CP_UTF8);
    if (out.empty()) {
        UINT cp = DetectCodepage(p.charset);
        out = ToWideFromCP(rawValue, cp);
        if (out.empty()) out = ToWideFromCP(rawValue, CP_ACP);
    }
    return trimW(out);
}

// ------ parse ------
std::vector<Contact> ParseVCard(const std::wstring& text) {
    std::vector<Contact> out;
    auto lines = Unfold(text);

    Contact cur;
    bool inCard = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string line = lines[i];
        if (line.empty()) continue;

        std::string upline = line; std::transform(upline.begin(), upline.end(), upline.begin(), ::toupper);

        if (upline.rfind("BEGIN:VCARD", 0) == 0) {
            inCard = true; cur = Contact{};
            continue;
        }
        if (upline.rfind("END:VCARD", 0) == 0) {
            if (inCard) out.push_back(cur);
            inCard = false;
            continue;
        }
        if (!inCard) continue;

        // split header:value (first ':')
        size_t col = line.find(':');
        std::string head = (col == std::string::npos) ? line : line.substr(0, col);
        std::string value = (col == std::string::npos) ? "" : line.substr(col + 1);

        std::string headUp = head; std::transform(headUp.begin(), headUp.end(), headUp.begin(), ::toupper);
        PropParams params = ParseParams(line);

        auto W = [&](const std::string& s)->std::wstring {
            return DecodeFieldText(s, params);
            };

        // PHOTO
        if (headUp.rfind("PHOTO", 0) == 0) {
            if (!params.encoding.empty()) {
                std::wstring enc = params.encoding; for (auto& c : enc) c = towupper(c);
                if (enc == L"B" || enc == L"BASE64") {
                    std::string b64 = value;
                    while (i + 1 < lines.size()) {
                        const std::string& nxt = lines[i + 1];
                        // stop if next line looks like a new property (contains ':' before any space)
                        size_t p = nxt.find(':');
                        size_t sc = nxt.find(';');
                        if (p != std::string::npos && (sc == std::string::npos || p < sc)) break;
                        // otherwise assume continuation of base64
                        b64 += nxt; ++i;
                    }
                    cur.photo = Photo{};
                    cur.photo->bytes = DecodeB64(b64);
                }
                else {
                    cur.photo_url = W(value);
                }
            }
            else {
                cur.photo_url = W(value);
            }
            continue;
        }

        // common fields
        if (headUp.rfind("FN", 0) == 0) { cur.fn = W(value); continue; }
        if (headUp.rfind("N:", 0) == 0 || headUp.rfind("N;", 0) == 0) {
            std::wstring w = W(value);
            size_t p1 = w.find(L';'); size_t p2 = std::wstring::npos;
            if (p1 != std::wstring::npos) { p2 = w.find(L';', p1 + 1); }
            cur.n_family = (p1 == std::wstring::npos) ? w : w.substr(0, p1);
            cur.n_given = (p1 != std::wstring::npos && p2 != std::wstring::npos) ? w.substr(p1 + 1, p2 - p1 - 1) : L"";
            continue;
        }
        if (headUp.rfind("ORG", 0) == 0) { cur.org = W(value); continue; }
        if (headUp.rfind("TITLE", 0) == 0) { cur.title = W(value); continue; }
        if (headUp.rfind("URL", 0) == 0) { cur.url = W(value); continue; }
        if (headUp.rfind("BDAY", 0) == 0) { cur.bday = W(value); continue; }
        if (headUp.rfind("NOTE", 0) == 0) { cur.note = W(value); continue; }

        if (headUp.rfind("TEL", 0) == 0) {
            Phone p; p.number = W(value);
            p.types = params.types;
            cur.phones.push_back(p);
            continue;
        }
        if (headUp.rfind("EMAIL", 0) == 0) {
            Email e; e.addr = W(value);
            e.types = params.types;
            cur.emails.push_back(e);
            continue;
        }
        if (headUp.rfind("ADR", 0) == 0) {
            Address a; a.text = W(value);
            cur.addrs.push_back(a);
            continue;
        }
    }

    return out;
}
