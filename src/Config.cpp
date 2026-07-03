#include "Config.hpp"

std::string fileName;
bool        fileNameFromCli = false;
bool        verbose         = false;
bool        overwrite       = false;

int     mtu     = 0;
int     ttl     = 0;
int     ttl_max = 0;
int64_t pace_us = 0;

SOCKET _socket;

SOCKADDR_IN server_address    = {};
SOCKADDR_IN client_address    = {};
SOCKADDR_IN broadcast_address = {};

addr_len server_address_length = sizeof(server_address);
addr_len client_address_length = sizeof(client_address);

size_t file_length = 0;
char*  file        = nullptr;
char*  buffer      = nullptr;
