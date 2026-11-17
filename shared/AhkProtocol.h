#pragma once
#ifndef NOVA_AHK_PROTOCOL_V1_H
#define NOVA_AHK_PROTOCOL_V1_H
#include <array>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Ahk {
inline constexpr unsigned CategoryCount=8, StatCount=13;
inline constexpr const char* Categories[]={"Dagger","Sword / Axe / Mace / Dual / Polearm","Katar","Zhen","Staff / Wand","Bow / Crossbow","Armor","Accessory"};
inline constexpr const char* Stats[]={"Strength","Dexterity","Intelligence","Psyche","Vitality","Agility","Health","Mana","Magic attack","Melee attack","Ranged attack","Physical defense","Magic defense"};
inline constexpr int StatIds[]={1,2,3,4,5,6,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d};
struct Row {
    std::uint16_t mask=0;
    std::array<std::int32_t,StatCount> minimum{};
};
struct Settings { bool active=false; std::array<Row,CategoryCount> rows{}; };
inline bool Valid(const Row& row) {
    if(row.mask & ~0x1fffu) return false;
    for(unsigned i=0;i<StatCount;++i)
        if((row.mask & (1u<<i)) && row.minimum[i]==0) return false;
    return true;
}
inline bool Valid(const Settings& value) {
    for(const auto& row:value.rows) if(!Valid(row)) return false;
    return true;
}
inline std::vector<std::string_view> Words(std::string_view text) {
    std::vector<std::string_view> result;
    while(!text.empty()) {
        const auto end=text.find(' ');
        const auto token=text.substr(0,end);
        if(token.empty()) return {};
        result.push_back(token);
        if(end==text.npos) break;
        text.remove_prefix(end+1);
        if(text.empty()) return {};
    }
    return result;
}
inline bool Number(std::string_view text,std::uint32_t& number) {
    if(text.empty()) return false;
    const auto r=std::from_chars(text.data(),text.data()+text.size(),number);
    return r.ec==std::errc{} && r.ptr==text.data()+text.size();
}
inline std::string Encode(const Row& row) {
    constexpr char digits[]="0123456789abcdef";
    std::string result;
    const auto put=[&](std::uint32_t value,unsigned n) {
        for(unsigned i=n;i;--i) result.push_back(digits[(value>>((i-1)*4))&15]);
    };
    put(row.mask,4);
    for(auto value:row.minimum) put(static_cast<std::uint32_t>(value),8);
    return result;
}
inline bool Decode(std::string_view text,Row& output) {
    if(text.size()!=4+8*StatCount) return false;
    const auto get=[&](unsigned offset,unsigned length,std::uint32_t& value) {
        const auto r=std::from_chars(text.data()+offset,text.data()+offset+length,value,16);
        return r.ec==std::errc{} && r.ptr==text.data()+offset+length;
    };
    Row row; std::uint32_t value=0;
    if(!get(0,4,value)) return false;
    row.mask=static_cast<std::uint16_t>(value);
    for(unsigned i=0;i<StatCount;++i) {
        if(!get(4+8*i,8,value)) return false;
        // Arithmetic conversion avoids implementation-defined unsigned -> signed overflow.
        row.minimum[i]=value<=0x7fffffffu?static_cast<std::int32_t>(value):
            static_cast<std::int32_t>(static_cast<std::int64_t>(value)-0x100000000LL);
    }
    if(!Valid(row)) return false;
    output=row; return true;
}
inline Settings Defaults() {
    Settings s;
    const auto set=[&](unsigned c,unsigned stat,int n) {s.rows[c].mask|=static_cast<std::uint16_t>(1u<<stat);s.rows[c].minimum[stat]=n;};
    for(unsigned stat:{2u,1u,5u,3u}) set(0,stat,30);
    set(0,6,10);set(0,9,10);
    set(1,2,30);set(1,6,10);set(1,9,10);
    set(2,5,30);set(2,6,10);set(2,9,10);
    set(3,6,10);set(3,9,10);set(3,8,10);
    set(4,2,30);set(4,3,30);set(4,6,10);set(4,8,10);
    set(5,10,10);set(5,6,10);set(6,11,30);
    for(unsigned i=0;i<6;++i) set(7,i,25);
    return s;
}
} // namespace Ahk
#endif // NOVA_AHK_PROTOCOL_V1_H
