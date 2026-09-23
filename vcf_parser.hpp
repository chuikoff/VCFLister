#pragma once
#include <string>
#include <vector>
#include <optional>
#include <stdint.h>

struct Phone {
    std::wstring number;
    std::vector<std::wstring> types;
};
struct Email {
    std::wstring addr;
    std::vector<std::wstring> types;
};
struct Address {
    std::wstring text;
};
struct Photo {
    std::vector<uint8_t> bytes; // embedded image
};

// One vCard property after decode — ready for UI (no re-parse of raw)
struct CardField {
    std::wstring prop;   // base name upper: FN, TEL, X-ABDATE, ...
    std::wstring head;   // full left side as in file (item2.EMAIL;TYPE=HOME)
    std::wstring value;  // decoded text (empty for binary PHOTO)
    bool show = true;    // false for VERSION/PHOTO blob/X-ABLABEL/noise
    bool isNote = false;
};

struct Contact {
    // name
    std::wstring fn;
    std::wstring n_family;
    std::wstring n_given;

    // org/role/etc
    std::wstring org;
    std::wstring title;
    std::wstring url;
    std::wstring bday;
    std::wstring note;

    // vCard 4.0
    std::wstring gender;
    std::wstring lang;
    std::wstring kind;
    std::vector<std::wstring> members;
    std::vector<std::wstring> langs;
    std::vector<std::wstring> urls;

    // comms
    std::vector<Phone>   phones;
    std::vector<Email>   emails;
    std::vector<Address> addrs;

    // photo
    std::optional<Photo> photo;
    std::wstring         photo_url;

    struct AndroidCustom {
        std::wstring rawType;
        std::vector<std::wstring> slots;
    };
    std::vector<std::wstring> notes;
    std::vector<AndroidCustom> androidCustoms;

    // All properties in file order (single parse → UI)
    std::vector<CardField> fields;
};

std::vector<Contact> ParseVCard(const std::wstring& text);
