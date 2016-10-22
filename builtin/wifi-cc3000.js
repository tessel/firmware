var util = require('util');
var EventEmitter = require('events').EventEmitter;

var hw = process.binding('hw');
var SECURITY_TYPES = ['wpa2', 'wpa', 'wep', 'unsecured'];

function Wifi(){
  var self = this;
  self.tryConnect = false;

  if (Wifi.instance) {
    return Wifi.instance;
  }
  else {
    Wifi.instance = this;
  }

  self.connect = function(options, callback){
    // options consists of 
    // { ssid: 
    //   , password: optional only if security == "unsecured"
    //   , security: defaults to wpa2 
    //   , timeout: defaults to 20s
    // }

    if (typeof(options) != 'object') {
      throw new Error("connect function takes in {ssid: '', password: '', security: ''}");
    }
    self.opts = options;
    if (!self.opts || !self.opts.ssid) {
      throw new Error("No SSID given");
    }

    self.opts.security = (self.opts.security && self.opts.security.toLowerCase()) || "wpa2";
    if (SECURITY_TYPES.indexOf(self.opts.security) == -1) {
      throw new Error(self.opts.security + " is not a supported security type. Supported types are 'wpa2', 'wpa', 'wep', and 'unsecured'");
    }

    self.opts.timeout = parseInt(self.opts.timeout) || 20;

    if (!self.opts.password && self.opts.security != "unsecured") {
      throw new Error("No password given for a network with security type "+ self.opts.security);
    }

    // initiate connection
    var ret = hw.wifi_connect(self.opts.ssid, self.opts.password, self.opts.security);
    var connectionTimeout;

    if (ret != 0) {
      var e = new Error("Previous wifi connect is in the middle of a call");
      self._failProcedure(e, callback);
      return self;
    } else {
      connectionTimeout = setTimeout(function(){
        self.emit('timeout', null);
        callback && callback("Connection timed out");
      }, self.opts.timeout * 1000);
    }

    if (callback) {
      self.once('connect', callback);
      clearTimeout(connectionTimeout);
    }

    if (!self.tryConnect) {
      // keep process open
      process.ref();
      self.tryConnect = true;
    }
    
    return self;
  };

  self._connectionCallback = function(err, data, next){
    next && next(err, data);
    if (!err) {
      try {
        self.emit('connect', JSON.parse(data));
      } catch (e) {
        self.emit('error', e);
      }
    } else {
      self.emit('disconnect', err);
    }
  };

  self.isConnected = function() {
    return hw.wifi_is_connected() == 1 ? true : false;
  };

  self.isBusy = function(){
    return hw.wifi_is_busy() == 1 ? true : false;
  };

  self.connection = function() {
    var data = JSON.parse(hw.wifi_connection());
    if (data.connected) {
      return data;
    }
    return null;
  };

  self.reset = function(callback) {
    // disable and then enable
    self.disable();
    self.enable();
    callback && callback();
    return self;
  };

  self.disconnect = function(callback){
    // disconnect
    var ret = hw.wifi_disconnect();

    if (ret != 0) {
      var e = new Error("Could not disconnect properly, wifi is currently busy.");
      self._failProcedure(e, callback);
      return self;
    }

    if (callback) {
      self.once('disconnect', function(){
        callback();
      });
    } 

    return self;
  };

  self._failProcedure = function (err, callback){
    setImmediate(function(){
      self.emit('error', err);
      if (callback) callback(err);
    });
  };

  self.isEnabled = function() {
    return hw.wifi_is_enabled() == 1 ? true : false;
  };

  self.disable = function(callback) {
    hw.wifi_disable();
    callback && callback();
    return self;
  };

  self.enable = function(callback) {
    hw.wifi_enable();
    callback && callback();
    return self;
  };

  self.macAddress = function() {
    // Gather the mac address
    return hw.wifi_mac_address();
  };

  // Once the script starts
  process.once('_script_running', function() {

    // If the Tessel is already connected
    if (self.isConnected()) {
      // go ahead and emit a connected event
      self.emit('connect', self.connection());
    } 

    // When we receive wifi_connect events, emit the 'connect' or 'error'
    // event to the user
    process.once('wifi_connect_complete', function(err, data){
        if (!err) {
          try {
            self._connectionCallback(err, data);

          } catch (e) {
            self.emit('error', e);
          }
        } else {
          self.emit('error', err);
        }
      });

    // When we receive a wifi_disconnect event, emit the 'disconnect' event to the user
    process.on('wifi_disconnect_complete', function(){
      self._connectionCallback("WiFi Disconnected");
    });

    // check for hangs
    process.on('wifi_hang', function(){
      self.emit('error', "Wifi chip is hanging");
    });
  });
}

util.inherits(Wifi, EventEmitter);

module.exports = new Wifi();
