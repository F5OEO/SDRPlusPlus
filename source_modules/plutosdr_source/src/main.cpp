#include <utils/flog.h>
#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <gui/style.h>
#include <gui/smgui.h>
#include <iio.h>
#include <ad9361.h>
#include <utils/optionlist.h>
#include <algorithm>
#include <regex>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "plutosdr_source",
    /* Description:     */ "PlutoSDR Tezuka source module for SDR++",
    /* Author:          */ "Ryzerth/F5OEO",
    /* Version:         */ 0, 2, 2,
    /* Max instances    */ 1
};

ConfigManager config;

const std::vector<const char*> deviceWhiteList = {
    "PlutoSDR",
    "ANTSDR",
    "LibreSDR",
    "Pluto+",
    "ad9361",
    "FISH"
};

class PlutoSDRSourceModule : public ModuleManager::Instance {
public:
    PlutoSDRSourceModule(std::string name) {
        this->name = name;

        // Define valid samplerates
        for (int sr = 2500000; sr <= 61440000; sr += 500000) {
            samplerates.define(sr, getBandwdithScaled(sr), sr);
        }
        samplerates.define(61440000, getBandwdithScaled(61440000.0), 61440000.0);

        // Define valid bandwidths
        bandwidths.define(0, "Auto", 0);
        for (int bw = 1000000.0; bw <= 52000000; bw += 500000) {
            bandwidths.define(bw, getBandwdithScaled(bw), bw);
        }

        // Define gain modes
        gainModes.define("manual", "Manual", "manual");
        gainModes.define("fast_attack", "Fast Attack", "fast_attack");
        gainModes.define("slow_attack", "Slow Attack", "slow_attack");
        gainModes.define("hybrid", "Hybrid", "hybrid");


        rfinputselect.define("rx1", "Rx1", "rx1");
        rfinputselect.define("rx2", "Rx2", "rx2");

        iqmodeselect.define("cs16", "CS16", "cs16");
        iqmodeselect.define("cs8", "CS8", "cs8");


        // Enumerate devices
        refresh();

        // Select device
        config.acquire();
        devDesc = config.conf["device"];
        config.release();
        select(devDesc);

        // Register source
        handler.ctx = this;
        handler.selectHandler = menuSelected;
        handler.deselectHandler = menuDeselected;
        handler.menuHandler = menuHandler;
        handler.startHandler = start;
        handler.stopHandler = stop;
        handler.tuneHandler = tune;
        handler.stream = &stream;
        sigpath::sourceManager.registerSource("PlutoSDR", &handler);
    }

    ~PlutoSDRSourceModule() {
        stop(this);
        sigpath::sourceManager.unregisterSource("PlutoSDR");
    }

    void postInit() {}

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = true;
    }

    bool isEnabled() {
        return enabled;
    }

private:
    std::string getBandwdithScaled(double bw) {
        char buf[1024];
        if (bw >= 1000000.0) {
            sprintf(buf, "%.1lfMHz", bw / 1000000.0);
        }
        else if (bw >= 1000.0) {
            sprintf(buf, "%.1lfKHz", bw / 1000.0);
        }
        else {
            sprintf(buf, "%.1lfHz", bw);
        }
        return std::string(buf);
    }

    void refresh() {
        // Clear device list
        devices.clear();

        // Create scan context
        iio_scan_context* sctx = iio_create_scan_context("usb:ip", 0);
        if (!sctx) {
            flog::error("Failed get scan context");
            return;
        }

        // Create parsing regexes
        std::regex backendRgx(".+(?=:)", std::regex::ECMAScript);
        std::regex modelRgx("\\(.+(?=\\),)", std::regex::ECMAScript);
        std::regex serialRgx("serial=[0-9A-Za-z]+", std::regex::ECMAScript);

        // Enumerate devices
        iio_context_info** ctxInfoList;
        ssize_t count = iio_scan_context_get_info_list(sctx, &ctxInfoList);
        if (count < 0) {
            flog::error("Failed to enumerate contexts");
            return;
        }
        for (ssize_t i = 0; i < count; i++) {
            iio_context_info* info = ctxInfoList[i];
            std::string desc = iio_context_info_get_description(info);
            std::string duri = iio_context_info_get_uri(info);

            // If the device is not a plutosdr, don't include it
            bool isPluto = false;
            for (const auto type : deviceWhiteList) {
                if (desc.find(type) != std::string::npos) {
                    isPluto = true;
                    break;
                }
            }
            if (!isPluto) {
                flog::warn("Ignored IIO device: [{}] {}", duri, desc);
                continue;
            }

            // Extract the backend
            std::string backend = "unknown";
            std::smatch backendMatch;
            if (std::regex_search(duri, backendMatch, backendRgx)) {
                backend = backendMatch[0];
            }

            // Extract the model
            std::string model = "Unknown";
            std::smatch modelMatch;
            if (std::regex_search(desc, modelMatch, modelRgx)) {
                model = modelMatch[0];
                int parenthPos = model.find('(');
                if (parenthPos != std::string::npos) {
                    model = model.substr(parenthPos + 1);
                }
            }

            // Extract the serial
            std::string serial = "unknown";
            std::smatch serialMatch;
            if (std::regex_search(desc, serialMatch, serialRgx)) {
                serial = serialMatch[0].str().substr(7);
            }

            // Construct the device name
            // std::string devName = '(' + backend + ") " + model + " [" + serial + ']';
            std::string devName = desc;
            if (devices.keyExists(desc) || devices.nameExists(devName) || devices.valueExists(duri)) { continue; }
            // Save device
            devices.define(desc, devName, duri);
        }
        iio_context_info_list_free(ctxInfoList);

        // Destroy scan context
        iio_scan_context_destroy(sctx);

#ifdef __ANDROID__
        // On Android, a default IP entry must be made (TODO: This is not ideal since the IP cannot be changed)
        const char* androidURI = "ip:192.168.2.1";
        const char* androidName = "Default (192.168.2.1)";
        devices.define(androidName, androidName, androidURI);
#endif
    }

    void select(const std::string& desc) {
        // If no device is available, give up
        if (devices.empty()) {
            devDesc.clear();
            return;
        }

        // If the device is not available, select the first one
        if (!devices.keyExists(desc)) {
            select(devices.key(0));
        }

        // Update URI
        devDesc = desc;
        uri = devices.value(devices.keyId(desc));

        // TODO: Enumerate capabilities

        // Load defaults
        samplerate = 4000000;
        bandwidth = 0;
        gmId = 0;
        gain = -1.0f;
        rfId = 0;
        iqmodeId = 0;

        // Load device config
        config.acquire();
        if (config.conf["devices"][devDesc].contains("samplerate")) {
            samplerate = config.conf["devices"][devDesc]["samplerate"];
        }
        if (config.conf["devices"][devDesc].contains("bandwidth")) {
            bandwidth = config.conf["devices"][devDesc]["bandwidth"];
        }
        if (config.conf["devices"][devDesc].contains("gainMode")) {
            // Select given gain mode or default if invalid
            std::string gm = config.conf["devices"][devDesc]["gainMode"];
            if (gainModes.keyExists(gm)) {
                gmId = gainModes.keyId(gm);
            }
            else {
                gmId = 0;
            }
        }
        if (config.conf["devices"][devDesc].contains("gain")) {
            gain = config.conf["devices"][devDesc]["gain"];
            gain = std::clamp<int>(gain, -1.0f, 73.0f);
        }

        if (config.conf["devices"][devDesc].contains("rfselect")) {
            // Select given gain mode or default if invalid
            std::string rf = config.conf["devices"][devDesc]["rfselect"];
            if (rfinputselect.keyExists(rf)) {
                rfId = rfinputselect.keyId(rf);
            }
            else {
                rfId = 0;
            }
        }

        config.release();

        // Update samplerate ID
        if (samplerates.keyExists(samplerate)) {
            srId = samplerates.keyId(samplerate);
        }
        else {
            srId = 0;
            samplerate = samplerates.value(srId);
        }

        // Update bandwidth ID
        if (bandwidths.keyExists(bandwidth)) {
            bwId = bandwidths.keyId(bandwidth);
        }
        else {
            bwId = 0;
            bandwidth = bandwidths.value(bwId);
        }
    }

    static void menuSelected(void* ctx) {
        PlutoSDRSourceModule* _this = (PlutoSDRSourceModule*)ctx;
        core::setInputSampleRate(_this->samplerate);
        flog::info("PlutoSDRSourceModule '{0}': Menu Select!", _this->name);
    }

    static void menuDeselected(void* ctx) {
        PlutoSDRSourceModule* _this = (PlutoSDRSourceModule*)ctx;
        flog::info("PlutoSDRSourceModule '{0}': Menu Deselect!", _this->name);
    }

    static void start(void* ctx) {
        PlutoSDRSourceModule* _this = (PlutoSDRSourceModule*)ctx;
        if (_this->running) { return; }

        // If no device is selected, give up
        if (_this->devDesc.empty() || _this->uri.empty()) { return; }

        // Open context
        _this->ctx = iio_create_context_from_uri(_this->uri.c_str());
        if (_this->ctx == NULL) {
            flog::error("Could not open pluto ({})", _this->uri);
            return;
        }

        // Get phy and device handle
        _this->phy = iio_context_find_device(_this->ctx, "ad9361-phy");
        if (_this->phy == NULL) {
            flog::error("Could not connect to pluto phy");
            iio_context_destroy(_this->ctx);
            return;
        }
        _this->dev = iio_context_find_device(_this->ctx, "cf-ad9361-lpc");
        if (_this->dev == NULL) {
            flog::error("Could not connect to pluto dev");
            iio_context_destroy(_this->ctx);
            return;
        }

        // Get RX channels
        long long Mode;
        iio_device_debug_attr_read_longlong(_this->phy, "adi,2rx-2tx-mode-enable", &Mode);
                
        if (Mode == 1)
                {  if(_this->rfId == 0) {
                         flog::info("RXCHAN0 '{0}': iqmode: {1}!", _this->name, _this->rfId);
                        _this->rxChan = iio_device_find_channel(_this->phy, "voltage0", false);
                    }
                    else {
                        
                         flog::info("RXCHAN0 '{0}': iqmode: {1}!", _this->name, _this->rfId);
                        _this->rxChan = iio_device_find_channel(_this->phy, "voltage1", false);
                    }
         }
         else
         {
                    uint32_t val = 0;
                    iio_device_reg_read(_this->phy, 0x00000003, &val);
                    val = (val & 0x3F) | ((_this->rfId + 1) << 6);
                    iio_device_reg_write(_this->phy, 0x00000003, val);

                    iio_device_debug_attr_write_longlong(_this->phy, "adi,1rx-1tx-mode-use-rx-num", _this->rfId + 1);
            
            _this->rxChan = iio_device_find_channel(_this->phy, "voltage0", false);
         }
        _this->rxLO = iio_device_find_channel(_this->phy, "altvoltage0", true);

        // Enable RX LO and disable TX
        iio_channel_attr_write_bool(iio_device_find_channel(_this->phy, "altvoltage1", true), "powerdown", true);
        iio_channel_attr_write_bool(_this->rxLO, "powerdown", false);

        // Configure RX channel
        iio_channel_attr_write(_this->rxChan, "rf_port_select", "A_BALANCED");
        iio_channel_attr_write_longlong(_this->rxLO, "frequency", round(_this->freq)); // Freq
        // iio_channel_attr_write_bool(_this->rxChan, "filter_fir_en", true);
        iio_channel_attr_write_longlong(_this->rxChan, "sampling_frequency", round(_this->samplerate));          // Sample rate
        iio_channel_attr_write_double(_this->rxChan, "hardwaregain", _this->gain);                               // Gain
        iio_channel_attr_write(_this->rxChan, "gain_control_mode", _this->gainModes.value(_this->gmId).c_str()); // Gain mode
        _this->setBandwidth(_this->bandwidth);

        // Configure the ADC filters
        // ad9361_set_bb_rate(_this->phy, round(_this->samplerate));

        // Start worker thread
        _this->running = true;
        _this->workerThread = std::thread(worker, _this);
        flog::info("PlutoSDRSourceModule '{0}': Start!", _this->name);
    }

    static void stop(void* ctx) {
        PlutoSDRSourceModule* _this = (PlutoSDRSourceModule*)ctx;
        if (!_this->running) { return; }

        // Stop worker thread
        _this->running = false;
        _this->stream.stopWriter();
        _this->workerThread.join();
        _this->stream.clearWriteStop();

        // Close device
        if (_this->ctx != NULL) {
            iio_context_destroy(_this->ctx);
            _this->ctx = NULL;
        }

        flog::info("PlutoSDRSourceModule '{0}': Stop!", _this->name);
    }

    static void tune(double freq, void* ctx) {
        PlutoSDRSourceModule* _this = (PlutoSDRSourceModule*)ctx;
        _this->freq = freq;
        if (_this->running) {
            // Tune device
            iio_channel_attr_write_longlong(_this->rxLO, "frequency", round(freq));
        }
        flog::info("PlutoSDRSourceModule '{0}': Tune: {1}!", _this->name, freq);
    }

    static void menuHandler(void* ctx) {
        PlutoSDRSourceModule* _this = (PlutoSDRSourceModule*)ctx;

        if (_this->running) { SmGui::BeginDisabled(); }
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo("##plutosdr_dev_sel", &_this->devId, _this->devices.txt)) {
            _this->select(_this->devices.key(_this->devId));
            core::setInputSampleRate(_this->samplerate);
            config.acquire();
            config.conf["device"] = _this->devices.key(_this->devId);
            config.release(true);
        }

        if (SmGui::Combo(CONCAT("##_pluto_sr_", _this->name), &_this->srId, _this->samplerates.txt)) {
            _this->samplerate = _this->samplerates.value(_this->srId);
            core::setInputSampleRate(_this->samplerate);
            if (!_this->devDesc.empty()) {
                config.acquire();
                config.conf["devices"][_this->devDesc]["samplerate"] = _this->samplerate;
                config.release(true);
            }
        }

        // Refresh button
        SmGui::SameLine();
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Button(CONCAT("Refresh##_pluto_refr_", _this->name))) {
            _this->refresh();
            _this->select(_this->devDesc);
            core::setInputSampleRate(_this->samplerate);
        }
        if (_this->running) { SmGui::EndDisabled(); }

        SmGui::LeftLabel("Bandwidth");
        SmGui::FillWidth();
        if (SmGui::Combo(CONCAT("##_pluto_bw_", _this->name), &_this->bwId, _this->bandwidths.txt)) {
            _this->bandwidth = _this->bandwidths.value(_this->bwId);
            if (_this->running) {
                _this->setBandwidth(_this->bandwidth);
            }
            if (!_this->devDesc.empty()) {
                config.acquire();
                config.conf["devices"][_this->devDesc]["bandwidth"] = _this->bandwidth;
                config.release(true);
            }
        }


        SmGui::LeftLabel("Gain Mode");
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##_pluto_gainmode_select_", _this->name), &_this->gmId, _this->gainModes.txt)) {
            if (_this->running) {
                 
                iio_channel_attr_write(_this->rxChan, "gain_control_mode", _this->gainModes.value(_this->gmId).c_str());
            }
            if (!_this->devDesc.empty()) {
                config.acquire();
                config.conf["devices"][_this->devDesc]["gainMode"] = _this->gainModes.key(_this->gmId);
                config.release(true);
            }
        }

        SmGui::LeftLabel("Gain");
        if (_this->gmId) { SmGui::BeginDisabled(); }
        SmGui::FillWidth();
        if (SmGui::SliderFloatWithSteps(CONCAT("##_pluto_gain__", _this->name), &_this->gain, -1.0f, 73.0f, 1.0f, SmGui::FMT_STR_FLOAT_DB_NO_DECIMAL)) {
            if (_this->running) {

                iio_channel_attr_write_double(_this->rxChan, "hardwaregain", _this->gain);
            }
            if (!_this->devDesc.empty()) {
                config.acquire();
                config.conf["devices"][_this->devDesc]["gain"] = _this->gain;
                config.release(true);
            }
        }
        if (_this->gmId) { SmGui::EndDisabled(); }
        if (_this->running) {   SmGui::BeginDisabled(); }
        SmGui::LeftLabel("RF input");
        
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##_pluto_rfinput_select_", _this->name), &_this->rfId, _this->rfinputselect.txt)) {
            
            if (_this->running)
             {
                uint32_t val = 0;

                long long Mode;
                iio_device_debug_attr_read_longlong(_this->phy, "adi,2rx-2tx-mode-enable", &Mode);
                
                if (Mode == 0) // Only if r11t
                {
                    iio_device_reg_read(_this->phy, 0x00000003, &val);
                    val = (val & 0x3F) | ((_this->rfId + 1) << 6);
                    iio_device_reg_write(_this->phy, 0x00000003, val);

                    iio_device_debug_attr_write_longlong(_this->phy, "adi,1rx-1tx-mode-use-rx-num", _this->rfId + 1);
                }
                else {
                    
                }
                // WorkAround because ad9361 driver doesnt reflect well rx change with gain
                // iio_channel_attr_write(iio_device_find_channel(_this->phy, "voltage0", false), "gain_control_mode", "fast_attack");
            }
            
            if (!_this->devDesc.empty()) {
                config.acquire();
                config.conf["devices"][_this->devDesc]["rfinput"] = _this->rfinputselect.key(_this->rfId);
                // config.conf["devices"][_this->devDesc]["rfinputselect"] = _this->rfinputselect.key(_this->gmId);
                config.release(true);
            }
            
        }
        if (_this->running) { SmGui::EndDisabled(); }
        if (_this->running) { SmGui::BeginDisabled(); }
        SmGui::LeftLabel("IQ Mode");
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##_pluto_iqmode_select_", _this->name), &_this->iqmodeId, _this->iqmodeselect.txt)) {
            if (_this->running) {
                // iio_channel_attr_write(_this->rxChan, "gain_control_mode", _this->rfinputselect.value(_this->gmId).c_str());
            }
            if (!_this->devDesc.empty()) {
                config.acquire();
                config.conf["devices"][_this->devDesc]["iqmode"] = _this->iqmodeselect.key(_this->iqmodeId);

                config.release(true);
            }
        }
        if (_this->running) { SmGui::EndDisabled(); }
        if (_this->running)
        {
            if(_this->underflow==1)
                SmGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Buffer : underflow");
            else
                SmGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Buffer : nominal");  
            if(_this->overgain==0)    
                SmGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Gain : OK");
            else
                SmGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Gain : overdrive");
        }    
        else
            SmGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Buffer : nominal");
    }

    void setBandwidth(int bw) {
        if (bw > 0) {
            iio_channel_attr_write_longlong(rxChan, "rf_bandwidth", bw);
        }
        else {
            iio_channel_attr_write_longlong(rxChan, "rf_bandwidth", std::min<int>(samplerate, 52000000));
        }
    }

    static void worker(void* ctx) {
        PlutoSDRSourceModule* _this = (PlutoSDRSourceModule*)ctx;
#define MAX_BUFFER_PLUTO 64000000 / 2
        size_t buffersize = ((size_t)(_this->samplerate / 20.0f));
        size_t blockSize;
        size_t nbkernel;

        blockSize = std::min<size_t>(STREAM_BUFFER_SIZE, buffersize);
        nbkernel = std::min<int>(8, MAX_BUFFER_PLUTO / blockSize);

        // Acquire channels
        iio_channel* rx0_i = iio_device_find_channel(_this->dev, "voltage0", 0);
        iio_channel* rx0_q = iio_device_find_channel(_this->dev, "voltage1", 0);

        long long Mode;
        iio_device_debug_attr_read_longlong(_this->phy, "adi,2rx-2tx-mode-enable", &Mode);
         flog::info("Failed to acquire RX channels");
        if ((_this->rfId == 1) && (Mode == 1)) {
            rx0_i = iio_device_find_channel(_this->dev, "voltage2", 0);
            rx0_q = iio_device_find_channel(_this->dev, "voltage3", 0);
        }

        if (!rx0_i || !rx0_q) {
            flog::error("Failed to acquire RX channels");
            return;
        }


        if (_this->iqmodeId == 0) {
            iio_channel_enable(rx0_i);
            iio_channel_enable(rx0_q);
        }
        else {
            iio_channel_enable(rx0_i);
            iio_channel_disable(rx0_q);
        }

        // Allocate buffer
        iio_device_set_kernel_buffers_count(_this->dev, nbkernel);

        flog::info("PlutoSDRSourceModule '{0}': Allocate {1} kernel buffers", _this->name, nbkernel);
        flog::info("PlutoSDRSourceModule '{0}': Allocate buffer size {1}", _this->name, blockSize);
        iio_buffer* rxbuf = iio_device_create_buffer(_this->dev, blockSize, false);
        // SetBUfferSize seems not working
        //_this->stream.setBufferSize(blockSize);
        if (!rxbuf) {
            flog::error("Could not create RX buffer");
            return;
        }

        uint32_t val = 0;
        iio_device_reg_read(_this->dev, 0x80000088, &val);
        iio_device_reg_write(_this->dev, 0x80000088, val); // Reset underflow state
        iio_device_reg_read(_this->dev, 0xC1200000, &val);
        flog::info("Plutosdr '{0}': Decim {1}", _this->name, val);

        // Receive loop
        while (true) {
            // Read samples
            ssize_t BufferRead = iio_buffer_refill(rxbuf);
            iio_device_reg_read(_this->dev, 0x80000088, &val);
            if (val & 4) {
                flog::warn("PlutoSDRSourceModule '{0}': Underflow!", _this->name);
                _this->underflow=1;
                iio_device_reg_write(_this->dev, 0x80000088, val);
            }
            else
                _this->underflow=0;

            if ((_this->rfId == 1) && (Mode == 1))    
                iio_device_reg_read(_this->phy, 0x0000005F, &val);    
            else
                iio_device_reg_read(_this->phy, 0x0000005E, &val);    
            _this->overgain=val&1;
            if(_this->overgain)
                flog::warn("PlutoSDRSourceModule '{0}': Overdive!", _this->name);
            if (_this->iqmodeId == 0) {
                int16_t* buf = (int16_t*)iio_buffer_start(rxbuf);
                if (buf) {
                    volk_16i_s32f_convert_32f((float*)_this->stream.writeBuf, buf, 2048.0f, blockSize * 2 /* BufferRead/(2*sizeof(int16_t))*/);
                    if (!_this->stream.swap(blockSize)) { break; };
                }
            }
            else {

                int8_t* buf = (int8_t*)iio_buffer_start(rxbuf);
                if (buf) {
                    volk_8i_s32f_convert_32f((float*)_this->stream.writeBuf, buf, 128.0f, blockSize * 2);
                    if (!_this->stream.swap(blockSize)) { break; };
                }
            }
        }

        // Stop streaming
        iio_channel_disable(rx0_i);
        iio_channel_disable(rx0_q);
        // flog::info("PlutoSDRSourceModule '{0}': iqmode: {1}!", _this->name, _this->iqmodeId);
        //  Free buffer
        iio_buffer_destroy(rxbuf);
    }

    std::string name;
    bool enabled = true;
    dsp::stream<dsp::complex_t> stream;
    SourceManager::SourceHandler handler;
    std::thread workerThread;
    iio_context* ctx = NULL;
    iio_device* phy = NULL;
    iio_device* dev = NULL;
    iio_channel* rxLO = NULL;
    iio_channel* rxChan = NULL;
    bool running = false;

    std::string devDesc = "";
    std::string uri = "";

    double freq;
    int samplerate = 4000000;
    int bandwidth = 0;
    float gain = -1;

    int devId = 0;
    int srId = 0;
    int bwId = 0;
    int gmId = 0;
    int rfId = 0;
    int iqmodeId = 0;
    int underflow=0;
    int overgain=0;

    OptionList<std::string, std::string> devices;
    OptionList<int, double> samplerates;
    OptionList<int, double> bandwidths;
    OptionList<std::string, std::string> gainModes;
    OptionList<std::string, std::string> rfinputselect;
    OptionList<std::string, std::string> iqmodeselect;
};

MOD_EXPORT void _INIT_() {
    json defConf = {};
    defConf["device"] = "";
    defConf["devices"] = {};
    config.setPath(core::args["root"].s() + "/plutosdr_source_config.json");
    config.load(defConf);
    config.enableAutoSave();

    // Reset the configuration if the old format is still used
    config.acquire();
    if (!config.conf.contains("device") || !config.conf.contains("devices")) {
        config.conf = defConf;
        config.release(true);
    }
    else {
        config.release();
    }
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new PlutoSDRSourceModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (PlutoSDRSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}