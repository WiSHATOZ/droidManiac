/*
  Post-process with https://babeljs.io/repl and https://javascript-minifier.com/
*/

const throttle = (func, wait) => {
  var ready = true;
  var args = null;
  return function throttled() {
    var context = this;
    if (ready) {
      ready = false;
      setTimeout(function () {
        ready = true;
        if (args) {
          throttled.apply(context);
        }
      }, wait);
      if (args) {
        func.apply(this, args);
        args = null;
      } else {
        func.apply(this, arguments);
      }
    } else {
      args = arguments;
    }
  };
};

// Element refs
var keys = document.getElementsByClassName("key");
var touchKeys = [];
var bottomKeys = touchKeys;

// ������������
const compileKey = (key) => {
  const prev = key.previousElementSibling;
  const next = key.nextElementSibling;
  return {
    top: key.offsetTop,
    bottom: key.offsetTop + key.offsetHeight,
    left: key.offsetLeft,
    right: key.offsetLeft + key.offsetWidth,
    kflag: parseInt(key.dataset.kflag) + (parseInt(key.dataset.air) ? 32 : 0),
    prevKeyRef: prev,
    nextKeyRef: next,
    ref: key,
  };
};

// �жϴ������Ƿ��ڰ�����
const isInside = (x, y, compiledKey) => {
  return (
    compiledKey.left <= x &&
    x < compiledKey.right &&
    compiledKey.top <= y &&
    y < compiledKey.bottom
  );
};

// ���밴����
const compileKeys = () => {
  keys = document.getElementsByClassName("key");
  // ��������Ԫ��
  touchKeys = [];
  for (var i = 0, key; i < keys.length; i++) {
    const compiledKey = compileKey(keys[i]);
    touchKeys.push(compiledKey);
  }
};

// �жϰ���
const getKey = (x, y) => {
    for (var i = 0; i < touchKeys.length; i++) {
        if (isInside(x, y, touchKeys[i])) {
            return touchKeys[i];
      }
    }
  return null;
};

// ����״̬
// prettier-ignore
var lastState = [
  0, 0, 0, 0,
];

//���´���
function updateTouches(e) {
  try {
    e.preventDefault();

    // prettier-ignore
    var keyFlags = [
      0, 0, 0, 0,
    ];

    //ȫ��
    throttledRequestFullscreen();

    for (var i = 0; i < e.touches.length; i++) {
      //��������
      const touch = e.touches[i];
      const x = touch.clientX;
      const y = touch.clientY;

      const key = getKey(x, y);
      if (!key) continue; // ���ڰ�����������
      setKey(keyFlags, key.kflag);
    }

      // ��Ⱦ��������
    for (var i = 0; i < touchKeys.length; i++) {
      const key = touchKeys[i];
      const kflag = key.kflag;
      if (keyFlags[kflag] !== lastState[kflag]) {
        if (keyFlags[kflag]) {
          key.ref.setAttribute("data-active", "");
        } else {
          key.ref.removeAttribute("data-active");
        }
      }
    }

      if (keyFlags !== lastState) {
        //���Ͱ�����Ϣ
      throttledSendKeys(keyFlags);
    }
    lastState = keyFlags;
  } catch (err) {
    alert (err);
  }
}
// ����Ƶ��
const throttledUpdateTouches = throttle(updateTouches, 10);

// ���ð���״̬
const setKey = (keyFlags, kflag) => {
  var idx = kflag;
  if (keyFlags[idx]) {
    idx++;
  }
  keyFlags[idx] = 1;
};

// ����
const sendKeys = (keyFlags) => {
  if (wsConnected) {
    ws.send("b" + keyFlags.join(""));
  }
};
const throttledSendKeys = throttle(sendKeys, 10);

// WebSocket
var ws = null;
var wsTimeout = 0;
var wsConnected = false;
const wsConnect = () => {
  ws = new WebSocket("ws://" + location.host + "/ws");
  ws.binaryType = "arraybuffer";
  ws.onopen = () => {
    ws.send("alive?");
  };
  ws.onmessage = (e) => {
    if (e.data.byteLength) {
      updateLed(e.data);
    } else if (e.data == "alive") {
      wsTimeout = 0;
      wsConnected = true;
    }
  };
};
const wsWatch = () => {
  if (wsTimeout++ > 2) {
    wsTimeout = 0;
    ws.close();
    wsConnected = false;
    wsConnect();
    return;
  }
  if (wsConnected) {
    ws.send("alive?");
  }
};

// canvas����
var canvas = document.getElementById("canvas");
var canvasCtx = canvas.getContext("2d");
var canvasData = canvasCtx.getImageData(0, 0, 5, 1);
const setupLed = () => {
  for (var i = 0; i < 5; i++) {
    canvasData.data[i * 4 + 3] = 255;
  }
};
setupLed();
const updateLed = (data) => {
  const buf = new Uint8Array(data);
  for (var i = 0; i < 4; i++) {
    canvasData.data[i * 4] = buf[(3 - i) * 3 + 1]; // r
    canvasData.data[i * 4 + 1] = buf[(3 - i) * 3 + 2]; // g
    canvasData.data[i * 4 + 2] = buf[(3 - i) * 3 + 0]; // b
  }
  // Copy from first led
  canvasData.data[128] = buf[94];
  canvasData.data[129] = buf[95];
  canvasData.data[130] = buf[93];
  canvasCtx.putImageData(canvasData, 0, 0);
};

// ȫ��
const fs = document.getElementById("fullscreen");
const requestFullscreen = () => {
  if (!document.fullscreenElement && screen.height <= 1024) {
    if (fs.requestFullscreen) {
      fs.requestFullscreen();
    } else if (fs.mozRequestFullScreen) {
      fs.mozRequestFullScreen();
    } else if (fs.webkitRequestFullScreen) {
      fs.webkitRequestFullScreen();
    }
  }
};
const throttledRequestFullscreen = throttle(requestFullscreen, 3000);

// Do update hooks
const cnt = document.getElementById("main");

cnt.addEventListener("touchstart", updateTouches);
cnt.addEventListener("touchmove", updateTouches);
cnt.addEventListener("touchend", updateTouches);

// ��������
const readConfig = (config) => {
  var style = "";

  if (!!config.invert) {
    style += `.container, .air-container {flex-flow: column-reverse nowrap;} `;
  }

  var bgColor = config.bgColor || "rbga(0, 0, 0, 0.9)";
  if (!config.bgImage) {
    style += `#fullscreen {background: ${bgColor};} `;
  } else {
    style += `#fullscreen {background: ${bgColor} url("${config.bgImage}") fixed center / cover!important; background-repeat: no-repeat;} `;
  }

  if (typeof config.ledOpacity === "number") {
    if (config.ledOpacity === 0) {
      style += `#canvas {display: none} `;
    } else {
      style += `#canvas {opacity: ${config.ledOpacity}} `;
    }
  }

  if (typeof config.keyColor === "string") {
    style += `.key[data-active] {background-color: ${config.keyColor};} `;
  }
  if (typeof config.keyBorderColor === "string") {
    style += `.key {border: 1px solid ${config.keyBorderColor};} `;
  }
  if (!!config.keyColorFade && typeof config.keyColorFade === "number") {
    style += `.key:not([data-active]) {transition: background ${config.keyColorFade}ms ease-out;} `;
  }

  if (typeof config.keyHeight === "number") {
    if (config.keyHeight === 0) {
      style += `.touch-container {display: none;} `;
    } else {
      style += `.touch-container {flex: ${config.keyHeight};} `;
    }
  }

  var styleRef = document.createElement("style");
  styleRef.innerHTML = style;
  document.head.appendChild(styleRef);
};

// ��ʼ��
const initialize = () => {
  readConfig(config);
  compileKeys();
  wsConnect();
  setInterval(wsWatch, 1000);
};
initialize();

// ������Сʱ���±��밴��
window.onresize = compileKeys;
