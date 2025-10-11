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

    // comms
    std::vector<Phone>   phones;
    std::vector<Email>   emails;
    std::vector<Address> addrs;

    // photo
    std::optional<Photo> photo;      // embedded
    std::wstring         photo_url;  // URL if provided
    // === multi NOTE + Android ===
    struct AndroidCustom {
        std::wstring rawType;              // строка до ':'
        std::vector<std::wstring> slots;   // части после ':', разделённые ';' (с учётом \;)
    };
    std::vector<std::wstring> notes;           // несколько NOTE
    std::vector<AndroidCustom> androidCustoms; // X-ANDROID-CUSTOM

};

std::vector<Contact> ParseVCard(const std::wstring& text);
