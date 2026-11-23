#include "labels.h"

#include <algorithm>
#include <cwctype>
#include <iterator>
#include <set>

namespace {

// Words that say nothing about which device it is: makers and generic product words (lower case).
const wchar_t* const kSkip[] = {
    L"logitech", L"logi", L"razer", L"steelseries", L"corsair", L"hyperx", L"glorious", L"keychron", L"microsoft",
    L"sony", L"asus", L"lamzu", L"pulsar", L"zowie", L"rapoo", L"wireless", L"gaming", L"mouse", L"headset",
    L"headphones", L"keyboard", L"controller", L"gamepad", L"receiver", L"bluetooth", L"usb",
};

bool Skipped(const std::wstring& word) {
    std::wstring lower = word;
    for (auto& c : lower) c = (wchar_t)towlower(c);
    return std::find_if(std::begin(kSkip), std::end(kSkip), [&](const wchar_t* s) { return lower == s; }) != std::end(kSkip);
}

std::vector<std::wstring> Words(const std::wstring& name) {
    std::vector<std::wstring> all, kept;
    std::wstring cur;
    for (wchar_t c : name) {
        if (iswspace(c)) {
            if (!cur.empty()) all.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) all.push_back(cur);
    for (const auto& w : all)
        if (!Skipped(w)) kept.push_back(w);
    return kept.empty() ? all : kept;
}

std::wstring Clip(std::wstring s, size_t maxChars) {
    if (s.size() > maxChars) s.resize(maxChars);
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    return s;
}

std::wstring FirstWords(const std::vector<std::wstring>& words, size_t k) {
    std::wstring s;
    for (size_t i = 0; i < k && i < words.size(); i++) s += (i ? L" " : L"") + words[i];
    return s;
}

bool AllDistinct(const std::vector<std::wstring>& v) {
    std::set<std::wstring> seen;
    for (const auto& s : v)
        if (s.empty() || !seen.insert(s).second) return false;
    return true;
}

bool IsNumber(const std::wstring& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
}

}  // namespace

std::vector<std::wstring> ShortLabels(const std::vector<LabelInput>& items, size_t maxChars) {
    std::vector<std::wstring> out(items.size());
    std::set<int> kinds;
    for (const auto& it : items) kinds.insert((int)it.kind);
    for (int kind : kinds) {
        std::vector<size_t> idx;
        for (size_t i = 0; i < items.size(); i++)
            if ((int)items[i].kind == kind) idx.push_back(i);
        if (idx.size() < 2) continue;

        std::vector<std::vector<std::wstring>> words;
        size_t most = 1;
        for (size_t i : idx) {
            words.push_back(Words(items[i].name));
            most = std::max(most, words.back().size());
        }

        std::vector<std::wstring> pick;
        for (size_t k = 1; k <= most && pick.empty(); k++) {
            std::vector<std::wstring> cand;
            for (const auto& w : words) cand.push_back(Clip(FirstWords(w, k), maxChars));
            if (AllDistinct(cand)) pick = cand;
        }
        if (pick.empty()) {  // same words all the way ("Xbox controller 1" / "Xbox controller 2")
            std::vector<std::wstring> nums;
            for (const auto& w : words) nums.push_back(!w.empty() && IsNumber(w.back()) ? Clip(w.back(), maxChars) : L"");
            if (AllDistinct(nums)) {
                pick = nums;
            } else {
                for (size_t n = 0; n < idx.size(); n++) pick.push_back(std::to_wstring(n + 1));
            }
        }
        for (size_t n = 0; n < idx.size(); n++) out[idx[n]] = pick[n];
    }
    return out;
}
