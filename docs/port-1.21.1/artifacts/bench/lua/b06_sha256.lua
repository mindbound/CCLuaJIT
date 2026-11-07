-- sha256: pure-Lua SHA-256 using bit32 (as in common CC crypto libraries)
local NAME = "sha256"
local ROUNDS = SHA_ROUNDS or 20
local band, bxor, bnot, rshift, rrotate = bit32.band, bit32.bxor, bit32.bnot, bit32.rshift, bit32.rrotate
local M32 = 4294967296
local K = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
}
local function pad(msg)
  local len = #msg
  local k = (55 - len) % 64
  local bits = len * 8
  local tail = {}
  for i = 7, 0, -1 do tail[#tail + 1] = string.char(math.floor(bits / 256 ^ i) % 256) end
  return msg .. "\128" .. string.rep("\0", k) .. table.concat(tail)
end
local function sha256(msg)
  msg = pad(msg)
  local H = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 }
  local w = {}
  for blk = 1, #msg, 64 do
    for j = 1, 16 do
      local p = blk + (j - 1) * 4
      local a, b, c, d = string.byte(msg, p, p + 3)
      w[j] = ((a * 256 + b) * 256 + c) * 256 + d
    end
    for j = 17, 64 do
      local v = w[j - 15]
      local s0 = bxor(rrotate(v, 7), rrotate(v, 18), rshift(v, 3))
      v = w[j - 2]
      local s1 = bxor(rrotate(v, 17), rrotate(v, 19), rshift(v, 10))
      w[j] = (w[j - 16] + s0 + w[j - 7] + s1) % M32
    end
    local a, b, c, d, e, f, g, h = H[1], H[2], H[3], H[4], H[5], H[6], H[7], H[8]
    for j = 1, 64 do
      local S1 = bxor(rrotate(e, 6), rrotate(e, 11), rrotate(e, 25))
      local ch = bxor(band(e, f), band(bnot(e), g))
      local t1 = (h + S1 + ch + K[j] + w[j]) % M32
      local S0 = bxor(rrotate(a, 2), rrotate(a, 13), rrotate(a, 22))
      local maj = bxor(band(a, b), band(a, c), band(b, c))
      local t2 = (S0 + maj) % M32
      h = g; g = f; f = e; e = (d + t1) % M32; d = c; c = b; b = a; a = (t1 + t2) % M32
    end
    H[1] = (H[1] + a) % M32; H[2] = (H[2] + b) % M32; H[3] = (H[3] + c) % M32; H[4] = (H[4] + d) % M32
    H[5] = (H[5] + e) % M32; H[6] = (H[6] + f) % M32; H[7] = (H[7] + g) % M32; H[8] = (H[8] + h) % M32
  end
  local out = {}
  for i = 1, 8 do out[i] = string.format("%08x", H[i] % M32) end
  return table.concat(out)
end
local MSG = {}
for i = 1, 4096 do MSG[i] = string.char((i * 7 + 3) % 256) end
MSG = table.concat(MSG)
local function run()
  assert(sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 self-test failed")
  local d = ""
  for r = 1, ROUNDS do d = sha256(MSG .. d) end
  return d
end
