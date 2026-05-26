// k6 run --vus 100 --duration 30s ws_test.js

import ws from 'k6/ws';
import { check } from 'k6';

export default function () {
  const url = 'ws://localhost:8080';
  const res = ws.connect(url, {}, function (socket) {
    socket.on('open', () => {
      socket.send('ping');
    });
    socket.on('message', (data) => {
      check(data, { 'message received': (d) => d === 'pong' });
      socket.close();
    });
  });
}