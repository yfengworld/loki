local core = require "sys.core"
local wsserver = require "saux.wsserver"

local handler = {}
function handler.on_open(ws)
    core.log(string.format("%d::open", ws.id))
end

function handler.on_message(ws, message)
    core.log(string.format("%d receive:%s", ws.id, message))
    ws:send_text(message .. "from server")
    --ws:close()
end

function handler.on_close(ws, code, reason)
    core.log(string.format("%d close:%s  %s", ws.id, code, reason))
end

wsserver.listen("192.168.1.65:9005", handler)

