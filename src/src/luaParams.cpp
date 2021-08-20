#ifdef TARGET_TX

#include "lua.h"

#include "CRSF.h"

const char thisCommit[] = {LATEST_COMMIT, 0};
const char thisVersion[] = {LATEST_VERSION, 0};
const char emptySpace[1] = {0};

struct tagLuaItem_textSelection luaAirRate = {
    {0,(uint8_t)CRSF_TEXT_SELECTION}, //id,type
    "P.Rate",
#if defined(Regulatory_Domain_AU_915) || defined(Regulatory_Domain_EU_868) || defined(Regulatory_Domain_FCC_915) || defined(Regulatory_Domain_IN_866) || defined(Regulatory_Domain_AU_433) || defined(Regulatory_Domain_EU_433) 
    "200(-112dbm);100(-117dbm);50(-120dbm);25(-123dbm)",
#elif defined(Regulatory_Domain_ISM_2400)
    "500(-105dbm);250(-108dbm);150(-112dbm);50(-117dbm)",
#endif
    {0,0,3},//value,min,max
    "Hz",
    LUA_TEXTSELECTION_SIZE(luaAirRate)
};
struct tagLuaItem_textSelection luaTlmRate = {
    {0,(uint8_t)CRSF_TEXT_SELECTION},//id,type
    "T.Rate",
    "off;1/128;1/64;1/32;1/16;1/8;1/4;1/2",
    {0,0,7},//value,min,max
    emptySpace,
    LUA_TEXTSELECTION_SIZE(luaTlmRate)
};
struct tagLuaItem_textSelection luaPower = {
    {0,(uint8_t)CRSF_TEXT_SELECTION},//id,type
    "Pwr",
    "10;25;50;100;250;500;1000;2000",
    {0,0,7},//value,min,max
    "mW",
    LUA_TEXTSELECTION_SIZE(luaPower)
};
struct tagLuaItem_textSelection luaSwitch = {
    {0,(uint8_t)CRSF_TEXT_SELECTION},//id,type
    "Switch",
    "1-Bit;Hybrid",
    {0,0,1},//value,min,max,default
    emptySpace,
    LUA_TEXTSELECTION_SIZE(luaSwitch)
};
struct tagLuaItem_textSelection luaModelMatch = {
    {5,(uint8_t)CRSF_TEXT_SELECTION},//id,type
    "Model Match",
    "Off;On",
    {0,0,1},//value,min,max
    emptySpace,
    LUA_TEXTSELECTION_SIZE(luaModelMatch)
};
struct tagLuaItem_uint8 luaSetRXModel = {
    {6,(uint8_t)CRSF_UINT8},//id,type
    "Set RX Model",
    {0,0,63},//value,min,max
    emptySpace,
    LUA_UINT8_SIZE(luaSetRXModel)
};
struct tagLuaItem_command luaBind = {
    {0,(uint8_t)CRSF_COMMAND},//id,type
    "Bind",
    {0,200},//status,timeout
    emptySpace,
    LUA_COMMAND_SIZE(luaBind)
};
struct tagLuaItem_string luaInfo = {
    {0,(uint8_t)CRSF_INFO},//id,type
    thisCommit,
    thisCommit,
    LUA_STRING_SIZE(luaInfo)
};
struct tagLuaItem_string luaELRSversion = {
    {0,(uint8_t)CRSF_INFO},//id,type
    thisVersion,
    emptySpace,
    LUA_STRING_SIZE(luaELRSversion)
};


#ifdef PLATFORM_ESP32
struct tagLuaItem_command luaWebUpdate = {
    {0,(uint8_t)CRSF_COMMAND},//id,type
    "Update",
    {0,200},//status,timeout
    emptySpace,
    LUA_COMMAND_SIZE(luaWebUpdate)
};
#endif
#endif
