const crypto = require('node:crypto');
const fs = require('node:fs');
// Generate independent DES-equivalent vectors using built-in TripleDES with K1=K2=K3.
function makeVector(username, password) {
  const key = Buffer.from(username.slice(-4) + username + '12345678', 'utf8').subarray(0, 8);
  const cipher = crypto.createCipheriv('des-ede3', Buffer.concat([key, key, key]), null);
  const expected = Buffer.concat([cipher.update(password, 'utf8'), cipher.final()]).toString('hex');
  return {username, password, expected};
}
fs.writeFileSync(process.argv[2], JSON.stringify([
  makeVector('2024123456', 'Test-Only-Password!'),
  makeVector('abc', '中文密码-仅测试'),
  makeVector('学生2468', '12345678'),
  makeVector('2024567890', '')
], null, 2));
