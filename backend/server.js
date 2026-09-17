require('dotenv').config();
const app = require('./app');

const PORT = process.env.PORT || 3000;
app.listen(PORT, () => {
  console.log(`ESP8266 IoT2 backend running on port ${PORT}`);
});
