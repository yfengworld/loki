local core = require "sys.core"
local socket = require "sys.socket"
local websocket = require "sys.websocket"
local stream = require "http.stream"

local listen = socket.listen
local readline = socket.readline
local read = socket.read
local write = socket.write

local assert = assert
local tonumber = tonumber
local sub = string.sub
local find = string.find
local format = string.format
local gmatch = string.gmatch
local insert = table.insert
local concat = table.concat

local wsserver = {}

local function parseuri(str)
	local form = {}
	local start = find(str, "?", 1, true)
	if not start then
		return str, form
	end
	assert(start > 1)
	local uri = sub(str, 1, start - 1)
	local f = sub(str, start + 1)
	for k, v in gmatch(f, "([^=&]+)=([^&]+)") do
		form[k] = v
	end
	return uri, form
end

local function httpwrite(fd, status, header, body)
	insert(header, 1, format("HTTP/1.1 %d %s", status, http_err_msg[status]))
	insert(header, format("Content-Length: %d", #body))
	local tmp = concat(header, "\r\n")
	tmp = tmp .. "\r\n\r\n"
	tmp = tmp .. body
	write(fd, tmp)
end

local function handle_socket(fd, handler)
    socket.limit(fd, 1024 * 512)
    while true do 
        -- limit request body size to 8192 (you can pass nil to unlimit)
        local status, first, header, body = stream.readrequest(fd, readline, read)
        if not status then	--disconnected
            return
        end
        if status ~= 200 then
            httpwrite(fd, status, {}, "")
            socket.close(fd)
            return
        end
        
        --request line
        local method, uri, ver = first:match("(%w+)%s+(.-)%s+HTTP/([%d|.]+)\r\n")
        assert(method and uri and ver)
        header.method = method
        header.version = ver
        header.uri, header.form = parseuri(uri)
        if tonumber(ver) > 1.1 then
            httpwrite(fd, 505, {}, "")
            socket.close(fd)
            return
        end
        if header["Content-Type"] == "application/x-www-form-urlencoded" then
            for k, v in gmatch(body, "(%w+)=(%w+)") do
                header.form[k] = v
            end
            body = ""
        end

        if header["Connection"] == "close" then
            socket.close(fd)
            return
        end

        if header["Upgrade"] == "websocket" then
            local ws = websocket.new(fd, header, handler)
            if ws then
                ws:start() 
            end
        end
    end 
end

local wsserver = {
	listen = function (addr, handler)
		local h = function(fd)
			handle_socket(fd, handler)
		end
		socket.listen(addr, h)
	end,
}

return wsserver






