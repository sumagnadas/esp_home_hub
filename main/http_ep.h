#include "esp_http_server.h"
#include "microlink_internal.h"

#define MAX_MACHINES_ENTRIES 16

typedef struct
{
    char ip[16];
    char name[100];
    uint8_t mac_addr[6];
    volatile int status;
    volatile int is_pinging;
} machine_t;

static const char WOL_PAGE_HTML[] =
    "<!DOCTYPE html>"
    "<html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>WoL selection</title>"
    "</head><body>"

    "<label for=\"wol-select\">Choose a machine to wake up</label><br>"
    "<select id=\"wol-select\" name=\"index\"><br>"
    "</select>"
    "<button onClick=\"wake()\">Wake</button>"

    "<script>"
    "machines=[];"
    "function renderMachines(){"
    "const el=document.getElementById(\"wol-select\");"
    "el.innerHTML=machines.map((p,i)=>"
    "'<option value='+i+'>'+p.name+'</option>'"
    ").join('');"
    "}"

    "async function loadMachines(){"
    "try{"
    "const r=await fetch('/status');"
    "const d=await r.json();"
    "machines=d.machines||[];"
    "renderMachines();"
    "}catch(e){}"
    "}"

    "async function wake(){"
    "try{"
    "const ind=parseInt(document.getElementById('wol-select').value.trim());"
    "await fetch('/wol',{method:'POST',headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({index:ind})});"
    "}catch(e){}}"
    "loadMachines();"
    "</script>"
    "</body></html>";

void start_webserver(microlink_t *);