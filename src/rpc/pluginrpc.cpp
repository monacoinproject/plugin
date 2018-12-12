// Copyright (c) 2018 Monacoinprojct

#include <plugin.h>
#include <rpc/client.h>
#include <rpc/protocol.h>
#include <rpc/server.h>
#include <util.h>
#include <utilstrencodings.h>

#include <set>
#include <stdint.h>

UniValue loadplugin(const JSONRPCRequest& request)
{
    std::string err;

    if (request.fHelp)
        throw std::runtime_error(
            "loadplugin filename\n"
            "\nLoad plug-ins into monacoind.\n"
            "\nArguments:\n"
            "1. \"filename\"   (string) filename under the directory of plugin.\n"
            "\nExamples:\n"
            + HelpExampleCli("loadplugin", "sample.lua")
        );

    std::string strFilename;
    if (request.params.size() > 0)
        strFilename = request.params[0].get_str();

    if(!plugin::LoadPlugin(strFilename.c_str()))
    {
        err =  "Error: Plugin, \"" + strFilename + "\", could not be loaded.";
        return err;
    }

    err =  "Success: Plugin, \"" + strFilename + "\", was loaded.";
    return err;
}


UniValue unloadplugin(const JSONRPCRequest& request)
{
    std::string err;

    if (request.fHelp)
        throw std::runtime_error(
            "unloadplugin filename\n"
            "\nUnload plug-ins from monacoind.\n"
            "\nArguments:\n"
            "1. \"filename\"   (string) filename under the directory of plugin.\n"
            "\nExamples:\n"
            + HelpExampleCli("unloadplugin", "sample.lua")
        );

    std::string strFilename;
    if (request.params.size() > 0)
        strFilename = request.params[0].get_str();

    if(!plugin::UnloadPlugin(strFilename.c_str()))
    {
        err =  "Error: Plugin, \"" + strFilename + "\", was not found on monacoind.";
        return err;
    }

    err =  "Success: Plugin, \"" + strFilename + "\", was unloaded.";
    return err;
}

static const CRPCCommand commands[] =
{ //  category              name                      actor (function)         argNames
  //  --------------------- ------------------------  -----------------------  ----------
    { "plugin",             "loadplugin",             &loadplugin,             {"filename"} },
    { "plugin",             "unloadplugin",           &unloadplugin,           {"filename"} },
};

void RegisterPluginRPCCommands(CRPCTable &t)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        t.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
