const http = require('http');
const fs = require('fs');
const path = require('path');
const WebSocket = require('ws');
const net = require('net');

// Configuration
const PORT = 3000;              // HTTP + WebSocket port
const TCP_HOST = '127.0.0.1';   // C++ Server IP
const TCP_PORT = 8000;          // C++ Server Port

// Create HTTP Server to serve index.html
const server = http.createServer((req, res) => {
    if (req.method === 'GET' && (req.url === '/' || req.url === '/index.html')) {
        fs.readFile(path.join(__dirname, 'index.html'), (err, data) => {
            if (err) {
                res.writeHead(500);
                res.end('Error loading index.html');
            } else {
                res.writeHead(200, { 'Content-Type': 'text/html' });
                res.end(data);
            }
        });
    } else {
        res.writeHead(404);
        res.end('Not Found');
    }
});

// Create WebSocket Server attached to HTTP server
const wss = new WebSocket.Server({ server });

console.log(`Web server running on http://localhost:${PORT}`);
console.log(`Forwarding to TCP server at ${TCP_HOST}:${TCP_PORT}`);

wss.on('connection', (ws) => {
    console.log('New WebSocket connection');

    // Create a TCP connection to the C++ server for this client
    const tcpClient = new net.Socket();
    
    tcpClient.connect(TCP_PORT, TCP_HOST, () => {
        console.log('Connected to C++ server');
    });

    // Buffer to handle TCP stream fragmentation/coalescing
    let buffer = Buffer.alloc(0);
    //servere给client发数据
    tcpClient.on('data', (data) => {
        buffer = Buffer.concat([buffer, data]);

        // Process all complete messages in the buffer
        while (true) {
            // Need at least 4 bytes for length header
            if (buffer.length < 4) {
                break;
            }

            // Read 4-byte Big-Endian length
            const length = buffer.readUInt32BE(0);

            // Check if we have the full message body
            if (buffer.length >= 4 + length) {
                // Extract message content
                const messageBuf = buffer.slice(4, 4 + length);
                const messageStr = messageBuf.toString('utf8');

                // Send to WebSocket client
                if (ws.readyState === WebSocket.OPEN) {
                    ws.send(messageStr);
                }

                // Remove processed message from buffer
                buffer = buffer.slice(4 + length);
            } else {
                // Wait for more data
                break;
            }
        }
    });

    tcpClient.on('error', (err) => {
        console.error('TCP Client Error:', err.message);
        ws.close();
    });

    tcpClient.on('close', () => {
        console.log('TCP connection closed');
        ws.close();
    });

    // Handle messages from WebSocket client
    ws.on('message', (message) => {
        // message is typically a Buffer or String. Convert to string to be safe.
        const msgStr = message.toString();
        
        console.log(`Sending to C++: ${msgStr}`);

        // Construct TCP packet: [4-byte Length][Content]
        const lengthBuf = Buffer.alloc(4);
        const contentBuf = Buffer.from(msgStr, 'utf8');
        
        lengthBuf.writeUInt32BE(contentBuf.length, 0);
        
        const packet = Buffer.concat([lengthBuf, contentBuf]);
        //client给server发数据
        tcpClient.write(packet);
    });

    ws.on('close', () => {
        console.log('WebSocket connection closed');
        tcpClient.end();
    });
});

// Start the server
server.listen(PORT, '0.0.0.0', () => {
    console.log(`Server listening on port ${PORT}`);
});
