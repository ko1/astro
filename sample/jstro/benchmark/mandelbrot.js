// Mandelbrot — heavy float arithmetic.  We don't print the bitmap; just
// count the iterations spent on it (this is the typical "mandelbrot-100"
// computational shape from the Are-We-Fast-Yet suite).
function mandelbrot(size) {
  var sum     = 0;
  var byteAcc = 0;
  var bitNum  = 0;

  var y = 0;
  while (y < size) {
    var ci = (2.0 * y / size) - 1.0;
    var x = 0;

    while (x < size) {
      var zr = 0.0;
      var zrzr = 0.0;
      var zi = 0.0;
      var zizi = 0.0;
      var cr = (2.0 * x / size) - 1.5;

      var z = 0;
      var notDone = true;
      var escape = 0;
      while (notDone && z < 50) {
        zr = zrzr - zizi + cr;
        zi = 2.0 * zr * zi + ci;

        zrzr = zr*zr;
        zizi = zi*zi;

        if (zrzr + zizi > 4.0) {
          notDone = false;
          escape = 1;
        }
        z = z + 1;
      }

      byteAcc = (byteAcc << 1) + escape;
      bitNum = bitNum + 1;

      if (bitNum === 8) {
        sum = sum ^ byteAcc;
        byteAcc = 0;
        bitNum  = 0;
      } else if (x === size - 1) {
        byteAcc = byteAcc << (8 - bitNum);
        sum = sum ^ byteAcc;
        byteAcc = 0;
        bitNum  = 0;
      }
      x = x + 1;
    }
    y = y + 1;
  }
  return sum;
}

var start = Date.now();
var size = 500;
var r = mandelbrot(size);
var elapsed = (Date.now() - start) / 1000;
console.log("mandelbrot(" + size + ") = " + r);
console.log("elapsed: " + elapsed + "s");
