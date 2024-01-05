const express = require('express');
const http = require('http');
const WebSocket = require('ws');
const { createCanvas } = require('canvas');
const { SerialPort } = require('serialport');
const { ReadlineParser } = require('@serialport/parser-readline');

const app = express();
const server = http.createServer(app);
const wss = new WebSocket.Server({ server });

const port = new SerialPort({ path: 'COM6', baudRate: 115200 });
const parser = port.pipe(new ReadlineParser({ delimiter: '\r\n' }));

let frameData = [];
let isCapturing = false;
let frameCount = 0;

const canvas = createCanvas(128, 128);
const ctx = canvas.getContext('2d');

function hexToColor16(hexValue) {
  const r5 = ((hexValue >> 11) & 0x1F);
  const g6 = ((hexValue >> 5) & 0x3F);
  const b5 = (hexValue & 0x1F);

  const r8 = Math.round((r5 / 31) * 255);
  const g8 = Math.round((g6 / 63) * 255);
  const b8 = Math.round((b5 / 31) * 255);

  return { r: r8, g: g8, b: b8 };
}

app.get('/', (req, res) => {
  res.sendFile(__dirname + '/index.html');
});

wss.on('connection', ws => {
  console.log('Client connected');

  parser.on('data', data => {
    const trimmedData = data.trim();

    if (trimmedData === 'FRAME_START') {
      isCapturing = true;
      frameData = [];
    } else if (trimmedData === 'FRAME_END') {
      isCapturing = false;

      const originalColors = frameData.map(hexValue => {
        const color = hexToColor16(parseInt(hexValue, 16));
        if (!color) {
          console.warn(`Invalid color data for hex value: ${hexValue}`);
        }
        return color;
      });

      for (let y = 0; y < canvas.height; y++) {
        for (let x = 0; x < canvas.width; x++) {
          const pixelIndex = y * canvas.width + x;
          const color = originalColors[pixelIndex];

          if (!color) {
            console.warn(`Undefined color at pixel index: ${pixelIndex}`);
            continue;
          }

          ctx.fillStyle = `rgb(${color.r}, ${color.g}, ${color.b})`;
          ctx.fillRect(x, y, 1, 1);
        }
      }

      const buffer = canvas.toBuffer();
      ws.send(buffer);
      frameCount++;
    } else if (isCapturing) {
      frameData.push(trimmedData);
    }
  });
});

port.on('error', err => {
  console.error('Error:', err.message);
});

port.open();

server.listen(3000, () => {
  console.log('Server running on http://localhost:3000');
});