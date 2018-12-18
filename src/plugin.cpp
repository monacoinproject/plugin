// Copyright (c) 2018 Monacoinprojct

#include <cstddef>
#include <fs.h>

#include <rpc/server.h>
#include <sync.h>
#include <univalue.h>
#include <util.h>
#include <utiltime.h>
#include <validation.h>

#include <lua/luasocket/luasocket.h>
#include <lua/luasocket/luasocket_scripts.h>
#include <lua/luasocket/mime.h>
#include <lua/luasocket/unix.h>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <plugin.h>

using namespace std;

//============================================================================
// external
//============================================================================

extern "C" {
int luaopen_cjson(lua_State *l);
}


namespace plugin {

//============================================================================
// prototype
//============================================================================

static bool CheckStack(lua_State *L, int n);

static bool lua_pushunivalue(lua_State *L, UniValue val);

static void CallWalletNotify(const std::string &hash);
static void CallBlockNotify(bool initialsync, const std::string &hash);

static void ExecLuaThread(lua_State *L, std::string funcname);

class CPluginQueue;

//============================================================================
// const value
//============================================================================

const size_t MAX_QUELE_DEPTH = 1024;
const int LUA_STACK_SIZE = 2048;

//============================================================================
// local work
//============================================================================

static map<string, CPlugin*> mapPlugins;
static CCriticalSection cs_plugin;

static std::thread threadQueue;
static CPluginQueue *pluginQueue = NULL;

static int th_counter = 0;
static std::map<int, std::thread> luaThread;
static int mtx_counter = 0;
static std::map<int, unique_ptr<std::mutex>> luaMutex;

//============================================================================
// UniValue to Lua
//============================================================================

static bool lua_pushunivalue_obj(lua_State *L, UniValue val)
{
    const std::vector<UniValue>& values = val.getValues();
    const std::vector<std::string>& keys = val.getKeys();

    if(!CheckStack(L, 3))
    {
        return false;
    }

    lua_newtable(L);

    for(unsigned int i = 0; i < keys.size(); i++)
    {
        // Push key
        lua_pushlstring(L, keys[i].c_str(), keys[i].length());
        // Fetch value
        lua_pushunivalue(L, values[i]);
        // Set key = value
        lua_rawset(L, -3);
    }
    
    return true;
}


static bool lua_pushunivalue_array(lua_State *L, UniValue val)
{
    bool ret = true;;
    const std::vector<UniValue>& values = val.getValues();

    if(!CheckStack(L, 2))
    {
        return false;
    }

    lua_newtable(L);

    for(unsigned int i = 0; i < values.size(); i++)
    {
        if(!lua_pushunivalue(L, values[i]))
        {
            return false;
        }
        lua_rawseti(L, -2, i);
    }

    return ret;
}

static bool lua_pushunivalue(lua_State *L, UniValue val)
{
    bool ret = true;;

    switch((int)val.getType())
    {
        case UniValue::VNULL:
            lua_pushlightuserdata(L, NULL);
            break;
        case UniValue::VOBJ:
            ret = lua_pushunivalue_obj(L, val);
            break;
        case UniValue::VARR:
            ret = lua_pushunivalue_array(L, val);
            break;
        case UniValue::VSTR:
            lua_pushlstring(L, val.get_str().c_str(), val.get_str().length());
            break;
        case UniValue::VNUM:
            lua_pushnumber(L, val.get_real());
            break;
        case UniValue::VBOOL:
            lua_pushboolean(L, val.get_bool());
            break;
    }

    return ret;
}


//============================================================================
// trim a std::string
//============================================================================

std::string& ltrim(std::string& str, const std::string& chars = "\t\n\v\f\r ")
{
    str.erase(0, str.find_first_not_of(chars));
    return str;
}

std::string& rtrim(std::string& str, const std::string& chars = "\t\n\v\f\r ")
{
    str.erase(str.find_last_not_of(chars) + 1);
    return str;
}
 
std::string& trim(std::string& str, const std::string& chars = "\t\n\v\f\r ")
{
    return ltrim(rtrim(str, chars), chars);
}

//============================================================================
// C++14 feature
//============================================================================

template<typename T, typename... Args>
std::unique_ptr<T> make_unique(Args&&... args) {
    return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

//============================================================================
// api func
//============================================================================

//----------------------------------------------------------------------------
// thread
//----------------------------------------------------------------------------

/*
 * l_CreateThread()
 */

static int l_CreateThread(lua_State *L)
{
    do{
        th_counter = (th_counter+1) & 0x7fffffff;
    } while(luaThread.find(th_counter) != luaThread.end());

    // thread func name
    std::string funcname = lua_tostring (L, 1);

    // create thread
    std::thread th(ExecLuaThread, L, funcname);

    // store thread info
    luaThread.insert(make_pair(th_counter, std::move(th)));

    lua_pushinteger(L, th_counter);
    return 1;
}

/*
 * l_Join()
 */

static int l_Join(lua_State *L)
{
    int th_no = (int)luaL_checkinteger(L, 1);

    std::map<int, std::thread>::iterator it = luaThread.find(th_no);
    if(it != luaThread.end())
    {

        if(it->second.joinable())
        {
            it->second.join();
        }

        luaThread.erase(it);
    }

    return 0;
}


/*
 * l_CreateMutex()
 */

static int l_CreateMutex(lua_State *L)
{
    do{
        mtx_counter = (mtx_counter+1) & 0x7fffffff;
    } while(luaMutex.find(mtx_counter) != luaMutex.end());

    // store mutex
    luaMutex.emplace(make_pair(mtx_counter, make_unique<std::mutex>()));

    lua_pushinteger(L, mtx_counter);
    return 1;
}


/*
 * l_DeleteMutex()
 */

static int l_DeleteMutex(lua_State *L)
{
    int mtx_no = (int)luaL_checkinteger(L, 1);

    std::map<int, unique_ptr<std::mutex>>::iterator it = luaMutex.find(mtx_no);
    if(it != luaMutex.end())
    {
        it->second.reset();
        luaMutex.erase(it);
    }

    return 0;
}


/*
 * l_Lock()
 */

static int l_Lock(lua_State *L)
{
    int mtx_no = (int)luaL_checkinteger(L, 1);

    std::map<int, unique_ptr<std::mutex>>::iterator it = luaMutex.find(mtx_no);
    if(it != luaMutex.end())
    {
        it->second->lock();
    }

    return 0;
}


/*
 * l_Unlock()
 */

static int l_Unlock(lua_State *L)
{
    int mtx_no = (int)luaL_checkinteger(L, 1);

    std::map<int, unique_ptr<std::mutex>>::iterator it = luaMutex.find(mtx_no);
    if(it != luaMutex.end())
    {
        it->second->unlock();
    }

    return 0;
}


/*
 * l_Sleep()
 */

static int l_Sleep(lua_State *L)
{

    int num = lua_gettop(L);
    if(num == 1)
    {
        MilliSleep((int)luaL_checkinteger(L, 1));
    }
    return 0;
}


//----------------------------------------------------------------------------
// coind internal
//----------------------------------------------------------------------------

/*
 * l_IsInitialBlockDownload()
 */

static int l_IsInitialBlockDownload(lua_State *L)
{
    lua_pushboolean(L, IsInitialBlockDownload());
    return 1;
}


//----------------------------------------------------------------------------
// wallet
//----------------------------------------------------------------------------

/*
 * l_getaccount()
 */

static int l_getaccount(lua_State *L)
{
    const char *address = luaL_checkstring (L, 1);

    CheckStack(L, 2);

    try {
        JSONRPCRequest request;
        request.strMethod = "getaccount";
        request.params.setArray();
        request.params.push_back(string(address));
        UniValue res = tableRPC.execute(request);

        lua_pushboolean(L, true);
        lua_pushstring(L, res.getValStr().c_str());
    } catch (const UniValue& e) {
        printf("%s\n", e.write().c_str());
        LogPrint(BCLog::PLUGIN, e.write().c_str());
        lua_pushboolean(L, false);
        lua_pushstring(L, "");
    }

    return 2;
}

/*
 * l_getaddressesbyaccount()
 */

static int l_getaddressesbyaccount(lua_State *L)
{
    const char *account = luaL_checkstring (L, 1);

    CheckStack(L, 2);

    try {
        JSONRPCRequest request;
        request.strMethod = "getaddressesbyaccount";
        request.params.setArray();
        request.params.push_back(string(account));
        UniValue res = tableRPC.execute(request);

        if(res.isNull())
        {
            lua_pushboolean(L, false);
            lua_pushstring(L, "");
        }
        else
        {
            lua_pushboolean(L, true);
            lua_pushunivalue(L, res);
        }
    } catch (const UniValue& e) {
        printf("%s\n", e.write().c_str());
        LogPrint(BCLog::PLUGIN, e.write().c_str());
        lua_pushboolean(L, false);
        lua_pushstring(L, "");
    }

    return 2;
}


/*
 * l_getbalance()
 */

static int l_getbalance(lua_State *L)
{
    UniValue tmpVal[3];
    JSONRPCRequest request;
    request.params.setArray();

    CheckStack(L, 2);

    int num = lua_gettop(L);
    switch(num)
    {
        case 3:                                                 // include_watchonly
            tmpVal[2].setBool(lua_toboolean(L, 3));
        case 2:                                                 // minconf
            tmpVal[1].setInt((int)luaL_checkinteger(L, 2));
        case 1:                                                 // account
            tmpVal[0].setStr(lua_tostring (L, 1));
            break;
        case 0:
            request.params.push_back("");
            break;
        default:
            lua_pushboolean(L, false);
            lua_pushnumber(L, 0.0);
            return 2;
    }

    request.strMethod = "getbalance";
    for(int i = 0; i < num; i++)
    {
        request.params.push_back(tmpVal[i]);
    }

    try {
        UniValue res = tableRPC.execute(request);
        lua_pushboolean(L, true);
        lua_pushnumber(L, res.get_real());
    } catch (const UniValue& e) {
        printf("%s\n", e.write().c_str());
        LogPrint(BCLog::PLUGIN, e.write().c_str());
        lua_pushboolean(L, false);
        lua_pushnumber(L, 0.0);
    }

    return 2;
}


/*
 * l_gettransaction()
 */

static int l_gettransaction(lua_State *L)
{
    UniValue tmpVal[2];
    JSONRPCRequest request;
    request.params.setArray();

    CheckStack(L, 2);

    int num = lua_gettop(L);
    switch(num)
    {
        case 2:                                                     // include_watchonly
            tmpVal[1].setBool(lua_toboolean(L, 2));
        case 1:
            tmpVal[0].setStr(lua_tostring (L, 1));                  // txid
            break;
        default:
            lua_pushboolean(L, false);
            lua_pushnumber(L, 0.0);
            return 2;
    }

    request.strMethod = "gettransaction";
    for(int i = 0; i < num; i++)
    {
        request.params.push_back(tmpVal[i]);
    }

    try {
        UniValue res = tableRPC.execute(request);
        lua_pushboolean(L, true);
        lua_pushunivalue(L, res);
    } catch (const UniValue& e) {
        printf("%s\n", e.write().c_str());
        LogPrint(BCLog::PLUGIN, e.write().c_str());
        lua_pushboolean(L, false);
        lua_pushstring(L, "");
    }

    return 2;

}


/*
 * l_sendfrom()
 */

static int l_sendfrom(lua_State *L)
{
    UniValue tmpVal[6];
    JSONRPCRequest request;
    request.params.setArray();

    CheckStack(L, 2);

    int num = lua_gettop(L);
    switch(num)
    {
        case 6:                                                     // comment_to
            tmpVal[5].setStr(lua_tostring (L, 6));
        case 5:                                                     // comment
            tmpVal[4].setStr(lua_tostring (L, 5));
        case 4:                                                     // minconf
            tmpVal[3].setInt((int)luaL_checkinteger(L, 4));
        case 3:
            tmpVal[2].setFloat((double)luaL_checknumber (L, 3));    // amount
            tmpVal[1].setStr(lua_tostring (L, 2));                  // toaddress
            tmpVal[0].setStr(lua_tostring (L, 1));                  // fromaccount
            break;
        default:
            lua_pushboolean(L, false);
            lua_pushnumber(L, 0.0);
            return 2;
    }

    request.strMethod = "sendfrom";
    for(int i = 0; i < num; i++)
    {
        request.params.push_back(tmpVal[i]);
    }

    try {
        UniValue res = tableRPC.execute(request);
        lua_pushboolean(L, true);
        lua_pushstring(L, res.getValStr().c_str());
    } catch (const UniValue& e) {
        printf("%s\n", e.write().c_str());
        LogPrint(BCLog::PLUGIN, e.write().c_str());
        lua_pushboolean(L, false);
        lua_pushstring(L, "");
    }

    return 2;
}


/*
 * l_sendmany()
 */

static int l_sendmany(lua_State *L)
{
    UniValue tmpVal[8];
    JSONRPCRequest request;
    request.params.setArray();

    CheckStack(L, 2);

    int num = lua_gettop(L);
    switch(num)
    {
        case 8:                                                     // estimate_mode
             tmpVal[7].setStr(lua_tostring (L, 8));
        case 7:                                                     // conf_target
            tmpVal[6].setInt((int)luaL_checkinteger(L, 7));
        case 6:                                                     // replaceable
             tmpVal[5].setBool(lua_toboolean(L, 6));
        case 5:                                                     // subtractfeefrom
             {
                 std::string str = lua_tostring (L, 5);
                 tmpVal[4].read(trim(str, "[]"));
             }
        case 4:                                                     // comment
             tmpVal[3].setStr(lua_tostring (L, 4));
        case 3:                                                     // minconf
            tmpVal[2].setInt((int)luaL_checkinteger(L, 3));
        case 2:
            {
                std::string str = lua_tostring (L, 2);
                tmpVal[1].read(str);                                // amounts
                tmpVal[0].setStr(lua_tostring (L, 1));              // fromaccount
            }
            break;
        default:
            lua_pushboolean(L, false);
            lua_pushnumber(L, 0.0);
            return 2;
    }

    request.strMethod = "sendmany";
    for(int i = 0; i < num; i++)
    {
        request.params.push_back(tmpVal[i]);
    }

    try {
        UniValue res = tableRPC.execute(request);
        lua_pushboolean(L, true);
        lua_pushstring(L, res.getValStr().c_str());
    } catch (const UniValue& e) {
        printf("%s\n", e.write().c_str());
        LogPrint(BCLog::PLUGIN, e.write().c_str());
        lua_pushboolean(L, false);
        lua_pushstring(L, "");
    }

    return 2;

}


/*
 * l_sendtoaddress()
 */

static int l_sendtoaddress(lua_State *L)
{
    UniValue tmpVal[8];
    JSONRPCRequest request;
    request.params.setArray();

    CheckStack(L, 2);

    int num = lua_gettop(L);
    switch(num)
    {
        case 8:                                                     // estimate_mode
            tmpVal[7].setStr(lua_tostring (L, 8));
        case 7:                                                     // conf_target
            tmpVal[6].setInt((int)luaL_checkinteger(L, 7));
        case 6:                                                     // replaceable
            tmpVal[5].setBool(lua_toboolean(L, 6));
        case 5:                                                     // subtractfeefromamount
            tmpVal[4].setBool(lua_toboolean(L, 5));
        case 4:                                                     // comment_to
            tmpVal[3].setStr(lua_tostring (L, 4));
        case 3:                                                     // comment
            tmpVal[2].setStr(lua_tostring (L, 3));
        case 2:
            tmpVal[1].setFloat((double)luaL_checknumber (L, 2));    // amount
            tmpVal[0].setStr(lua_tostring (L, 1));                  // address
            break;
        default:
            lua_pushboolean(L, false);
            lua_pushnumber(L, 0.0);
            return 2;
    }

    request.strMethod = "sendtoaddress";
    for(int i = 0; i < num; i++)
    {
        request.params.push_back(tmpVal[i]);
    }

    try {
        UniValue res = tableRPC.execute(request);
        lua_pushboolean(L, true);
        lua_pushstring(L, res.getValStr().c_str());
    } catch (const UniValue& e) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "");
    }

    return 2;
}


//----------------------------------------------------------------------------
// blockchain
//----------------------------------------------------------------------------

/*
 * l_getblock()
 */

static int l_getblock(lua_State *L)
{
    UniValue tmpVal[2];
    JSONRPCRequest request;
    request.params.setArray();

    CheckStack(L, 2);

    int num = lua_gettop(L);
    switch(num)
    {
        case 2:                                                 // verbosity
            tmpVal[1].setInt((int)luaL_checkinteger(L, 2));
        case 1:                                                 // blockhash
            tmpVal[0].setStr(lua_tostring (L, 1));
            break;
        default:
            lua_pushboolean(L, false);
            lua_pushnumber(L, 0.0);
            return 2;
    }

    request.strMethod = "getblock";
    for(int i = 0; i < num; i++)
    {
        request.params.push_back(tmpVal[i]);
    }

    try {
        UniValue res = tableRPC.execute(request);
        lua_pushboolean(L, true);
        lua_pushunivalue(L, res);
    } catch (const UniValue& e) {
        printf("%s\n", e.write().c_str());
        LogPrint(BCLog::PLUGIN, e.write().c_str());
        lua_pushboolean(L, false);
        lua_pushnumber(L, 0.0);
    }

    return 2;

}


/*
 * l_gettxoutproof()
 */

static int l_gettxoutproof(lua_State *L)
{
    UniValue tmpVal[2];
    JSONRPCRequest request;
    request.params.setArray();

    CheckStack(L, 2);

    int num = lua_gettop(L);
    switch(num)
    {
        case 2:                                                 // blockhash
            tmpVal[1].setStr(lua_tostring (L, 2));
        case 1:                                                 // txids
            {
                std::string str = lua_tostring (L, 1);
                tmpVal[0].read(str);
            }
            break;
        default:
            lua_pushboolean(L, false);
            lua_pushnumber(L, 0.0);
            return 2;
    }

    request.strMethod = "gettxoutproof";
    for(int i = 0; i < num; i++)
    {
        request.params.push_back(tmpVal[i]);
    }

    try {
        UniValue res = tableRPC.execute(request);
        lua_pushboolean(L, true);
        lua_pushunivalue(L, res);
    } catch (const UniValue& e) {
        printf("%s\n", e.write().c_str());
        LogPrint(BCLog::PLUGIN, e.write().c_str());
        lua_pushboolean(L, false);
        lua_pushnumber(L, 0.0);
    }

    return 2;
}



/*
 * l_verifytxoutproof()
 */

static int l_verifytxoutproof(lua_State *L)
{
    CheckStack(L, 2);

    int num = lua_gettop(L);
    if(num != 1)
    {
        lua_pushboolean(L, false);
        lua_pushstring(L, "");
        return 2;
    }

    const char *proof = luaL_checkstring (L, 1);

    JSONRPCRequest request;
    request.strMethod = "verifytxoutproof";
    request.params.setArray();
    request.params.push_back(string(proof));

    try {
        UniValue res = tableRPC.execute(request);
        if(res.isNull())
        {
            lua_pushboolean(L, false);
            lua_pushstring(L, "");
        }
        else
        {
            lua_pushboolean(L, true);
            lua_pushunivalue(L, res);
        }
    } catch (const UniValue& e) {
        printf("%s\n", e.write().c_str());
        LogPrint(BCLog::PLUGIN, e.write().c_str());
        lua_pushboolean(L, false);
        lua_pushnumber(L, 0.0);
    }

    return 2;
}


//----------------------------------------------------------------------------
// rawtransaction
//----------------------------------------------------------------------------


/*
 * l_fundrawtransaction()
 */

static int l_fundrawtransaction(lua_State *L)
{
    UniValue tmpVal[3];
    JSONRPCRequest request;
    request.params.setArray();

    CheckStack(L, 2);

    int num = lua_gettop(L);
    switch(num)
    {
        case 3:                                                 // iswitness
            tmpVal[2].setBool(lua_toboolean(L, 3));
        case 2:                                                 // options
            {
                std::string str = lua_tostring (L, 2);
                tmpVal[1].read(str);
            }
        case 1:                                                 // The hex string of the raw transaction
            tmpVal[0].setStr(lua_tostring (L, 1));
            break;
        default:
            lua_pushboolean(L, false);
            lua_pushnumber(L, 0.0);
            return 2;
    }

    request.strMethod = "fundrawtransaction";
    for(int i = 0; i < num; i++)
    {
        request.params.push_back(tmpVal[i]);
    }

    try {
        UniValue res = tableRPC.execute(request);
        lua_pushboolean(L, true);
        lua_pushunivalue(L, res);
    } catch (const UniValue& e) {
        printf("%s\n", e.write().c_str());
        LogPrint(BCLog::PLUGIN, e.write().c_str());
        lua_pushboolean(L, false);
        lua_pushnumber(L, 0.0);
    }

    return 2;
}


/*
 * l_getrawtransaction()
 */

static int l_getrawtransaction(lua_State *L)
{
    bool verbose = false;
    UniValue tmpVal[3];
    JSONRPCRequest request;
    request.params.setArray();

    CheckStack(L, 2);

    int num = lua_gettop(L);
    switch(num)
    {
        case 3:                                                 // blockhash
            tmpVal[2].setStr(lua_tostring (L, 3));
        case 2:                                                 // verbose
            verbose = lua_toboolean(L, 2);
            tmpVal[1].setBool(verbose);
        case 1:                                                 // txid
            tmpVal[0].setStr(lua_tostring (L, 1));
            break;
        default:
            lua_pushboolean(L, false);
            lua_pushnumber(L, 0.0);
            return 2;
    }

    request.strMethod = "getrawtransaction";
    for(int i = 0; i < num; i++)
    {
        request.params.push_back(tmpVal[i]);
    }

    try {
        UniValue res = tableRPC.execute(request);
        lua_pushboolean(L, true);
        if(verbose)
        {
	        lua_pushunivalue(L, res);
        }
        else
        {
	        lua_pushstring(L, res.getValStr().c_str());
        }
    } catch (const UniValue& e) {
        printf("%s\n", e.write().c_str());
        LogPrint(BCLog::PLUGIN, e.write().c_str());
        lua_pushboolean(L, false);
        lua_pushnumber(L, 0.0);
    }

    return 2;
}


/*
 * l_createrawtransaction()
 */

static int l_createrawtransaction(lua_State *L)
{
    UniValue tmpVal[4];
    JSONRPCRequest request;
    request.params.setArray();

    CheckStack(L, 2);

    int num = lua_gettop(L);
    switch(num)
    {
        case 4:                                                 // replaceable
            tmpVal[3].setBool(lua_toboolean(L, 2));
        case 3:                                                 // locktime
            tmpVal[2].setInt((int)luaL_checkinteger(L, 3));
        case 2:
            {
                std::string str = lua_tostring (L, 2);          // outputs
                tmpVal[1].read(str);
                str = lua_tostring (L, 1);                      // inputs
                tmpVal[0].read(str);
            }
            break;
        default:
            lua_pushboolean(L, false);
            lua_pushnumber(L, 0.0);
            return 2;
    }

    request.strMethod = "createrawtransaction";
    for(int i = 0; i < num; i++)
    {
        request.params.push_back(tmpVal[i]);
    }

    try {
        UniValue res = tableRPC.execute(request);
        lua_pushboolean(L, true);
        lua_pushstring(L, res.getValStr().c_str());
    } catch (const UniValue& e) {
        printf("%s\n", e.write().c_str());
        LogPrint(BCLog::PLUGIN, e.write().c_str());
        lua_pushboolean(L, false);
        lua_pushnumber(L, 0.0);
    }

    return 2;
}


/*
 * l_decoderawtransaction()
 */

static int l_decoderawtransaction(lua_State *L)
{
    UniValue tmpVal[2];
    JSONRPCRequest request;
    request.params.setArray();

    CheckStack(L, 2);

    int num = lua_gettop(L);
    switch(num)
    {
        case 2:                                                 // iswitness
            tmpVal[1].setBool(lua_toboolean(L, 2));
        case 1:                                                 // transaction hex string
            tmpVal[0].setStr(lua_tostring (L, 1));
            break;
        default:
            lua_pushboolean(L, false);
            lua_pushnumber(L, 0.0);
            return 2;
    }

    request.strMethod = "decoderawtransaction";
    for(int i = 0; i < num; i++)
    {
        request.params.push_back(tmpVal[i]);
    }

    try {
        UniValue res = tableRPC.execute(request);
        lua_pushboolean(L, true);
        lua_pushunivalue(L, res);
    } catch (const UniValue& e) {
        printf("%s\n", e.write().c_str());
        LogPrint(BCLog::PLUGIN, e.write().c_str());
        lua_pushboolean(L, false);
        lua_pushnumber(L, 0.0);
    }

    return 2;
}


/*
 * l_decodescript()
 */

static int l_decodescript(lua_State *L)
{
    CheckStack(L, 2);

    int num = lua_gettop(L);
    if(num != 1)
    {
        lua_pushboolean(L, false);
        lua_pushstring(L, "");
        return 2;
    }

    const char *hexstring = luaL_checkstring (L, 1);

    JSONRPCRequest request;
    request.strMethod = "decodescript";
    request.params.setArray();
    request.params.push_back(string(hexstring));

    try {
        UniValue res = tableRPC.execute(request);
        lua_pushboolean(L, true);
        lua_pushunivalue(L, res);
    } catch (const UniValue& e) {
        printf("%s\n", e.write().c_str());
        LogPrint(BCLog::PLUGIN, e.write().c_str());
        lua_pushboolean(L, false);
        lua_pushstring(L, "");
    }

    return 2;
}


/*
 * l_sendrawtransaction()
 */

static int l_sendrawtransaction(lua_State *L)
{
    UniValue tmpVal[2];
    JSONRPCRequest request;
    request.params.setArray();

    CheckStack(L, 2);

    int num = lua_gettop(L);
    switch(num)
    {
        case 2:                                                 // Allow high fees
            tmpVal[1].setBool(lua_toboolean(L, 2));
        case 1:                                                 // The hex string of the raw transaction
            tmpVal[0].setStr(lua_tostring (L, 1));
            break;
        default:
            lua_pushboolean(L, false);
            lua_pushnumber(L, 0.0);
            return 2;
    }

    request.strMethod = "sendrawtransaction";
    for(int i = 0; i < num; i++)
    {
        request.params.push_back(tmpVal[i]);
    }

    try {
        UniValue res = tableRPC.execute(request);
        lua_pushboolean(L, true);
        lua_pushstring(L, res.getValStr().c_str());
    } catch (const UniValue& e) {
        printf("%s\n", e.write().c_str());
        LogPrint(BCLog::PLUGIN, e.write().c_str());
        lua_pushboolean(L, false);
        lua_pushnumber(L, 0.0);
    }

    return 2;
}


/*
 * l_signrawtransaction()
 */

static int l_signrawtransaction(lua_State *L)
{
    UniValue tmpVal[4];
    JSONRPCRequest request;
    request.params.setArray();

    CheckStack(L, 2);

    int num = lua_gettop(L);
    switch(num)
    {
        case 4:                                                 // sighashtype
            tmpVal[3].setStr(lua_tostring (L, 4));
        case 3:                                                 // privkeys
            {
                std::string str = lua_tostring (L, 3);
                tmpVal[2].read(str);
            }
        case 2:                                                 // An json array of previous dependent transaction outputs
            {
                std::string str = lua_tostring (L, 2);
                if(str.empty())
                {
                    tmpVal[1].setNull();
                }
                else
                {
	                tmpVal[1].read(str);
	            }
            }
        case 1:                                                 // The transaction hex string
            tmpVal[0].setStr(lua_tostring (L, 1));
            break;
        default:
            lua_pushboolean(L, false);
            lua_pushnumber(L, 0.0);
            return 2;
    }

    request.strMethod = "signrawtransaction";
    for(int i = 0; i < num; i++)
    {
        request.params.push_back(tmpVal[i]);
    }

    try {
        UniValue res = tableRPC.execute(request);
        lua_pushboolean(L, true);
        lua_pushunivalue(L, res);
    } catch (const UniValue& e) {
        printf("%s\n", e.write().c_str());
        LogPrint(BCLog::PLUGIN, e.write().c_str());
        lua_pushboolean(L, false);
        lua_pushnumber(L, 0.0);
    }

    return 2;
}


/*
 * l_combinerawtransaction()
 */

static int l_combinerawtransaction(lua_State *L)
{
	UniValue tmpVal;

    CheckStack(L, 2);

    int num = lua_gettop(L);
    if(num != 1)
    {
        lua_pushboolean(L, false);
        lua_pushstring(L, "");
        return 2;
    }

    std::string str = lua_tostring (L, 1);
    tmpVal.read(str);

    JSONRPCRequest request;
    request.strMethod = "combinerawtransaction";
    request.params.setArray();
    request.params.push_back(tmpVal);

    try {
        UniValue res = tableRPC.execute(request);
        lua_pushboolean(L, true);
        lua_pushstring(L, res.getValStr().c_str());
    } catch (const UniValue& e) {
        printf("%s\n", e.write().c_str());
        LogPrint(BCLog::PLUGIN, e.write().c_str());
        lua_pushboolean(L, false);
        lua_pushstring(L, "");
    }

    return 2;
}


//----------------------------------------------------------------------------
// RegisterCoindFunc
//----------------------------------------------------------------------------

/*
 * function table
 */

static const struct luaL_Reg coindApi [] = {
    // thread func
    {"CreateThread",           l_CreateThread},
    {"Join",                   l_Join},
    {"CreateMutex",            l_CreateMutex},
    {"DeleteMutex",            l_DeleteMutex},
    {"Lock",                   l_Lock},
    {"Unlock",                 l_Unlock},
    {"Sleep",                  l_Sleep},

    // coind internal func
    {"IsInitialBlockDownload", l_IsInitialBlockDownload},

    // wallet
    {"getaccount",             l_getaccount},
    {"getaddressesbyaccount",  l_getaddressesbyaccount},
    {"getbalance",             l_getbalance},
    {"gettransaction",         l_gettransaction},
    {"sendfrom",               l_sendfrom},
    {"sendmany",               l_sendmany},
    {"sendtoaddress",          l_sendtoaddress},

    // blockchain
    {"getblock",               l_getblock},
    {"gettxoutproof",          l_gettxoutproof},
    {"verifytxoutproof",       l_verifytxoutproof},

    // rawtransaction
    {"fundrawtransaction",     l_fundrawtransaction },
    {"getrawtransaction",      l_getrawtransaction},
    {"createrawtransaction",   l_createrawtransaction},
    {"decoderawtransaction",   l_decoderawtransaction},
    {"decodescript",           l_decodescript},
    {"sendrawtransaction",     l_sendrawtransaction},
    {"combinerawtransaction",  l_combinerawtransaction},
    {"signrawtransaction",     l_signrawtransaction},

    {NULL, NULL}
};

static int RegisterCoindFunc(lua_State* L)
{
    luaL_newlib(L, coindApi);

    return 1;
}

/*
 * extension
 */

static struct luaL_Reg lua_extension [] = {
    {"cjson",        luaopen_cjson},
    {"socket.unix",  luaopen_socket_unix},
    {"socket.core",  luaopen_socket_core},
    {"socket.core",  luaopen_socket_core},
    {"mime.core",    luaopen_mime_core},
    {NULL, NULL}
};

static int RegisterExtension(lua_State* L)
{
    luaL_Reg* lib = lua_extension;
    for (; lib->func; lib++)
    {
        luaL_requiref(L, lib->name, lib->func, 1);
        lua_pop(L, 1);
    }

    return 1;
}


//============================================================================
// class CPlugin
//============================================================================


/*
 * CPlugin::CPlugin()
 */

CPlugin::CPlugin()
{
    L = NULL;
}


/*
 * CPlugin::~CPlugin()
 */

CPlugin::~CPlugin()
{
    Unload();
}


/*
 * CPlugin::Load()
 */
bool CPlugin::Load(const char* filename)
{
    if(L)
    {
        return false;
    }

    fs::path pathPlugin = GetDataDir() / "plugin" / filename;

    L = luaL_newstate();
    CheckStack(L, LUA_STACK_SIZE);
    luaL_openlibs(L);

    // register lua extension
    RegisterExtension(L);
    luaopen_luasocket_scripts(L);

    // register api
    luaL_requiref(L, "coind", RegisterCoindFunc, 1);

    if( luaL_loadfile(L, pathPlugin.generic_string().c_str()) == LUA_ERRFILE )
    {
        lua_close(L);
        L = NULL;
        return false;
    }
    lua_pcall(L, 0, 0, 0);

    // call OnInit()
    lua_getglobal(L, "OnInit");
    if (lua_isfunction(L, -1)) {
        if(lua_pcall(L, 0, 0, 0))
        {
            lua_close(L);
            L = NULL;
            return false;
        }
    }
    else
    {
        lua_pop(L,1); 
    }

    return true;
}


/*
 * CPlugin::Unload()
 */

void CPlugin::Unload()
{
    if(L)
    {
        // call OnTerm()
        lua_getglobal(L, "OnTerm");
        if (lua_isfunction(L, -1)) {
            lua_pcall(L, 0, 0, 0);
        }
        else
        {
            lua_pop(L,1); 
        }

        // close
        lua_close(L);
        L = NULL;
    }
}


/*
 * CPlugin::WalletNotify()
 */

void CPlugin::WalletNotify(std::string hash)
{
    lua_State *co = lua_newthread(L);

    // call OnWalletNotify()
    lua_getglobal(co, "OnWalletNotify");
    if (lua_isfunction(co, -1)) {
        lua_pushstring(co, hash.c_str());
        lua_pcall(co, 1, 0, 0);
    }
    else
    {
        lua_pop(co,1); 
    }

}


/*
 * CPlugin::BlockNotify()
 */

void CPlugin::BlockNotify(bool initialsync, std::string hash)
{
    lua_State *co = lua_newthread(L);

    // call OnBlockNotify()
    lua_getglobal(co, "OnBlockNotify");
    if (lua_isfunction(co, -1)) {
        lua_pushboolean(co, initialsync);
        lua_pushstring(co, hash.c_str());
        lua_pcall(co, 2, 0, 0);
    }
    else
    {
        lua_pop(co,1); 
    }
}


//============================================================================
// class CPluginQueue
//============================================================================

class CPluginQueue
{
private:
    std::mutex cs;
    std::condition_variable cond;
    std::deque<UniValue*> queue;
    bool running;
    size_t maxDepth;
    bool queuewait;

    enum NotifyType {
        NONE,
        BLOCK_NOTIFY,
        WALLET_NOTIFY
    };


public:
    explicit CPluginQueue(size_t _maxDepth) : running(true), maxDepth(_maxDepth)
    {
        queuewait = gArgs.GetBoolArg("-queuewait", true);
    }

    ~CPluginQueue()
    {
    }

    bool PushBlockNotify(bool initialsync, std::string hash)
    {
        UniValue *val = new UniValue(UniValue::VARR);
        if(!val)
        {
            return false;
        }

        val->push_back((int)BLOCK_NOTIFY);
        val->push_back(initialsync);
        val->push_back(hash);
        Enqueue(val);
       return true;
    }

    bool PushWalletNotify(std::string hash)
    {
        UniValue *val = new UniValue(UniValue::VARR);
        if(!val)
        {
            return false;
        }

        val->push_back((int)WALLET_NOTIFY);
        val->push_back(hash);
        Enqueue(val);
        return true;
    }


    bool Enqueue(UniValue *value)
    {
        std::unique_lock<std::mutex> lock(cs);
        if(queuewait)
        {
            while(queue.size() >= maxDepth)
            {
                if(!running)
                {
                    return false;
                }
                MilliSleep(100);
            }
        }
        else
        {
            if(queue.size() >= maxDepth || !running)
            {
                return false;
            }
        }

        queue.emplace_back(value);
        cond.notify_one();
        return true;
    }

    void Run()
    {
        UniValue *val;
        while (true) {
            {
                std::unique_lock<std::mutex> lock(cs);
                while (running && queue.empty())
                    cond.wait(lock);
                if (!running && queue.empty())
                    break;
                val = queue.front();
                queue.pop_front();
            }

            const std::vector<UniValue>& vecVal = val->getValues();
            switch((NotifyType)vecVal[0].get_int())
            {
                case BLOCK_NOTIFY:
                    CallBlockNotify(vecVal[1].getBool(), vecVal[2].getValStr());
                    break;
                case WALLET_NOTIFY:
                    CallWalletNotify(vecVal[1].get_str());
                    break;
                default:
                    break;
            }
            delete(val);
        }
    }

    void Interrupt()
    {
        std::unique_lock<std::mutex> lock(cs);
        running = false;
        cond.notify_all();
    }
};


//============================================================================
// local function
//============================================================================

/*
 * CheckStack()
 */


static bool CheckStack(lua_State *L, int n)
{
    if (lua_checkstack(L, n))
        return true;

    LogPrint(BCLog::PLUGIN, "Too many nested data structures\n");
    return false;
}


/*
 * PluginQueueRun()
 */

static void PluginQueueRun(CPluginQueue *queue)
{
    RenameThread("monacoin-pluginworker");
    queue->Run();
}


/*
 * CallWalletNotify()
 */

static void CallWalletNotify(const std::string &hash)
{
    LOCK(cs_plugin);
    map<string, CPlugin*>::iterator me = mapPlugins.end();
    for (map<string, CPlugin*>::iterator mi = mapPlugins.begin(); mi != me; mi++)
    {
        mi->second->WalletNotify(hash);
    }

}


/*
 * CallBlockNotify()
 */

static void CallBlockNotify(bool initialsync, const std::string &hash)
{
    LOCK(cs_plugin);
    map<string, CPlugin*>::iterator me = mapPlugins.end();
    for (map<string, CPlugin*>::iterator mi = mapPlugins.begin(); mi != me; mi++)
    {
      mi->second->BlockNotify(initialsync, hash);
    }
}

/*
 * ExecLuaThread()
 */

static void ExecLuaThread(lua_State *L, std::string funcname)
{
    lua_State *co = lua_newthread(L);

    // call lua func
    lua_getglobal(co, funcname.c_str());
    if (lua_isfunction(co, -1)) {
        lua_pcall(co, 0, 0, 0);
    }
    else
    {
        lua_pop(co,1); 
    }
}

//============================================================================
// global function
//============================================================================


/*
 * Init()
 */

void Init()
{
    if(pluginQueue)
    {
        return;
    }
    
    size_t depth = gArgs.GetArg("-queuedepth", MAX_QUELE_DEPTH);
    pluginQueue = new CPluginQueue(depth);
    threadQueue = std::thread(PluginQueueRun, pluginQueue);
}



/*
 * Term()
 */

void Term()
{
    if(pluginQueue)
    {
        pluginQueue->Interrupt();
        threadQueue.join();
        delete pluginQueue;
        pluginQueue = NULL;
    }
    
    map<string, CPlugin*>::iterator me = mapPlugins.end();
    for (map<string, CPlugin*>::iterator mi = mapPlugins.begin(); mi != me; mi++)
    {
        delete(mi->second);
    }
    mapPlugins.clear();
}


/*
 * LoadPlugin()
 */

bool LoadPlugin(const char *filename)
{
    LOCK(cs_plugin);
    if(mapPlugins.count(filename) != 0)
    {
        return false;
    }

    CPlugin *pl = new CPlugin();
    if(pl->Load(filename))
    {
        mapPlugins.insert(make_pair(string(filename), pl));
        return true;
    }

    delete(pl);
    return false;
}


/*
 * UnloadPlugin()
 */

bool UnloadPlugin(const char *filename)
{
    LOCK(cs_plugin);
    map<string, CPlugin*>::iterator mi = mapPlugins.find(filename);
    if(mi == mapPlugins.end())
    {
        return false;
    }
    delete(mi->second);
    mapPlugins.erase(mi);

    return true;
}


/*
 * WalletNotify()
 */

void WalletNotify(const std::string &hash)
{
    if(pluginQueue)
    {
        pluginQueue->PushWalletNotify(hash);
    }
}


/*
 * BlockNotify()
 */

void BlockNotify(bool initialsync, const std::string &hash)
{
    if(pluginQueue)
    {
        pluginQueue->PushBlockNotify(initialsync, hash);
    }
}


}
