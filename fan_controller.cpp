/**
 * Fan Controller ---
 * This is to help optimize use of fans in the winter, with a couple goals:
 * 1) Keep the furnace blower fan running longer after heat.
 *    - My furnace still has useful heat for longer than the 2 minute max
 *      runtime the furnace circuit bloard let me set.
 * 2) Turn up my ceiling fans when the furnace is on.
 *    - I have high ceilings and the heat comes in from the ceiling.
 *      The furnace blower needs some help to get that heat down to
 *      where I want it!
 * 3) Turn the ceiling fans back down after the furnace turns on.
 *     - For my use, I want to keep the fans going on the lowest setting.
 *       I definitely don't want them turned up higher!
 * 4) Delay the ceiling fan adjustments
 *     - There's a delay from the call-for-heat until we have warm air
 *       to circulate down.
 *     - Keep the fans running for a bit until the air the furnace is
 *       blowing out isn't warm enough.
 *
 * TODO
 * 1) split up the code
 *    - I started as a single file, which makes it easy to compile
 *    - It's borderline too big for one file
 *    - Some functionality might be useful for other purposes
 * 2) Read in some parameters from a config file
 *    - Might be able to re-use the same logic for other seasons,
 *      and of course avoid recompiling just to make a tweak.
 */

#include <curl/curl.h>
#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <syslog.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#undef DEBUG

namespace {
static const auto k_thermostatPollFrequencySeconds = std::chrono::seconds(15);
static const auto k_runBlowerFanAfterHeatOff = std::chrono::seconds(60 * 6);
static const auto k_ceilingFanOnDelay = std::chrono::seconds(60);
static const auto k_ceilingFanOffDelay = std::chrono::seconds(180);
static const int k_heatOnFanSpeed = 2;
static const int k_heatOffFanSpeed = 1;
/*
 *  By default CURL does not timeout http requests. We started with this at 4 seconds,
 *  thinking 3 should be more than enough for the simple requests performed. However,
 *  we saw many instances where this took much longer (need to analyze that!), so we've
 *  bumped this up for the time being.
 */
static const int k_httpTimeout = 10;  // seconds

static const int BLOWER_ON = 2;

class CurlObj {
  CURL* curl;

 public:
  CurlObj(const std::string& url) {
    curl = curl_easy_init();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

    // Don't bother trying IPv6, which would increase DNS resolution time.
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, k_httpTimeout);  // default is forever
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    // All our messages to the devices use JSON data.  Although we did find that our
    // devices don't seem to care if we set this or not, examples typically did set it.
    struct curl_slist* headers = NULL;
    curl_slist_append(headers, "Content-Type: application/json");
    curl_slist_append(headers, "charset: utf-8");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  }
  ~CurlObj() { curl_easy_cleanup(curl); }
  CURL* operator()() { return curl; }
};

std::string GetURL(CURL* c) {
  const char* urlStr = NULL;
  CURLcode rc = curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &urlStr);
  if (!urlStr) return std::string();
  return std::string(urlStr);
}

// Current state of data from the thermostat that we care about
struct ThermostatState {
  float temp;
  float targetTemp;
  bool isHeatOn;
  int blowerState;  // 0 = AUTO, 1 = CIRCULATE, 2 = ON

  ThermostatState(const float temp, const float targetTemp, const bool isHeatOn,
                  const int blowerState)
      : temp(temp), targetTemp(targetTemp), isHeatOn(isHeatOn), blowerState(blowerState) {}
};
std::ostream& operator<<(std::ostream& os, const ThermostatState& currentState) {
  os << "State: Temp: " << currentState.temp << " Target: " << currentState.targetTemp
     << " Heat On: " << currentState.isHeatOn << " Blower: " << currentState.blowerState;
  return os;
}

class Thermostat final {
  CURL* curlInstance;
  std::optional<ThermostatState> previousState;
  std::chrono::steady_clock::time_point lastTransitionTime;
  bool stateChanged;
  unsigned long failCount;

  void SetState(const ThermostatState& newState);
  std::optional<ThermostatState> ParseState(const std::string& stateData);

  friend std::ostream& operator<<(std::ostream& os, const Thermostat& currentState);

 public:
  Thermostat(CURL* curlInstance);
  ~Thermostat();

  // Returns the time since the furnace last turned on or turned off, or zero is we haven't yet seen
  // a transition.
  auto GetTimeSinceTransition() const;

  // Returns true iff we were able to successfully retrieve and parse the new state data from the
  // thermostat.
  bool Update();

  void Debug();

  // True if the furnace mode (off, heat, cool) changed since the last update.
  bool StateChanged() const;

  bool isFurnaceOn() const;

  // \return the last known blower state, or -1 if we haven't fetched thermostat data yet.
  int GetBlowerState() const;
};

class Fan {
 protected:
  CURL* curlInstance;

 public:
  Fan(CURL* inst) : curlInstance(inst) {}
  virtual ~Fan() {}
  virtual void Update(const Thermostat& tstat) = 0;
  virtual void Debug() = 0;
};

class FurnaceBlower : public Fan {
  std::optional<int> latchedState;

 public:
  FurnaceBlower(CURL*);
  ~FurnaceBlower();
  void Update(const Thermostat& tstat) final;
  void Debug() final;
  bool SetBlowerState(int newState);
};

class CeilingFan : public Fan {
  bool fanStateUpdatedSinceLastTransition;

 public:
  CeilingFan(CURL*);
  ~CeilingFan();
  void Update(const Thermostat& tstat) final;
  void Debug() final;
  bool SetFanSpeed(int speed);
  int GetFanSpeed();
  void Reboot();
};

void writeJsonOut(const rapidjson::Document& doc) {
  rapidjson::StringBuffer sb;
  rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(sb);
  doc.Accept(writer);
  std::cout << "\nJSON data received:" << std::endl;
  std::cout << sb.GetString() << std::endl;
}

std::size_t callback(const char* in, std::size_t size, std::size_t num, std::string* out) {
  const std::size_t totalBytes(size * num);
  if (out) out->assign(in, totalBytes);
  return totalBytes;
}

/**
 * Return the data from an HTTP request.  If an error in the return code, an empty string.
 */
std::pair<long, std::string> doHttpRequest(CURL* curlInstance) {
  std::string result;
  curl_easy_setopt(curlInstance, CURLOPT_WRITEFUNCTION, callback);
  curl_easy_setopt(curlInstance, CURLOPT_WRITEDATA, &result);
  curl_easy_perform(curlInstance);
  long httpReturnCode(0);
  curl_easy_getinfo(curlInstance, CURLINFO_RESPONSE_CODE, &httpReturnCode);
  return std::make_pair(httpReturnCode, result);
}

Thermostat::Thermostat(CURL* curlInstance)
    : curlInstance(curlInstance),
      previousState(std::nullopt),
      lastTransitionTime(std::chrono::steady_clock::now() - k_runBlowerFanAfterHeatOff),
      stateChanged(false),
      failCount(0) {}

Thermostat::~Thermostat() {}

// Returns the time since the funace last turned on or turned off, or zero is we haven't yet seen a
// transition.
auto Thermostat::GetTimeSinceTransition() const {
  if (lastTransitionTime.time_since_epoch() == std::chrono::steady_clock::duration::zero()) {
    return std::chrono::steady_clock::duration::zero();
  }
  return std::chrono::steady_clock::now() - lastTransitionTime;
}

std::optional<ThermostatState> Thermostat::ParseState(const std::string& thermostatData) {
  if (thermostatData.empty()) {
    std::cerr << "Empty thermostat data returned!" << std::endl;
    return std::nullopt;
  }

  rapidjson::Document jsonDoc;
  jsonDoc.Parse(thermostatData.c_str());
#ifdef DEBUG
  writeJsonOut(jsonDoc);
#endif
  if (jsonDoc.HasParseError()) {
    std::cerr << "Error parsing thermostat data: " << thermostatData << std::endl;
    return std::nullopt;
  }
  if (!jsonDoc.HasMember("temp") || !jsonDoc.HasMember("t_heat") || !jsonDoc.HasMember("tstate") ||
      !jsonDoc.HasMember("fmode")) {
    std::cerr << "Missing fields in thermostat data: " << thermostatData << std::endl;
    return std::nullopt;
  }

  return ThermostatState{jsonDoc["temp"].GetFloat(), jsonDoc["t_heat"].GetFloat(),
                         jsonDoc["tstate"].GetInt() == 1, jsonDoc["fmode"].GetInt()};
}

bool Thermostat::Update() {
  stateChanged = false;
  curl_easy_setopt(curlInstance, CURLOPT_HTTPGET, 1L);
  auto thermostatData = doHttpRequest(curlInstance);
  if (thermostatData.first != 200) {
    std::cerr << "Thermostat returned error code: " << thermostatData.first << std::endl;

    if (++failCount % 6 == 0)
      syslog(LOG_ERR,
             "Thermostat %s failed to get data %lu attempts. Returned code: %ld, response: %s",
             GetURL(curlInstance).c_str(), failCount, thermostatData.first,
             thermostatData.second.c_str());
    return false;
  }

  std::optional<ThermostatState> newState = ParseState(thermostatData.second);
  if (!newState) {
    if (++failCount % 6 == 0)
      syslog(LOG_ERR,
             "Thermostat %s failed to parse data %lu attempts. Returned code: %ld, response: %s",
             GetURL(curlInstance).c_str(), failCount, thermostatData.first,
             thermostatData.second.c_str());
    return false;
  }
  failCount = 0;

  stateChanged = previousState && newState->isHeatOn != previousState->isHeatOn;
  previousState = *newState;
  if (stateChanged) lastTransitionTime = std::chrono::steady_clock::now();

  return true;
}

void Thermostat::Debug() {
  curl_easy_setopt(curlInstance, CURLOPT_HTTPGET, 1L);
  auto thermostatData = doHttpRequest(curlInstance);
  std::cout << "Thermostat response: " << thermostatData.first << std::endl
            << thermostatData.second << std::endl
            << std::endl;
}

// True if the furnace mode (off, heat, cool) changed since the last update.
bool Thermostat::StateChanged() const { return stateChanged; }

bool Thermostat::isFurnaceOn() const { return previousState && previousState->isHeatOn; }

// \return the last known blower state, or -1 if we haven't fetched thermostat data yet.
int Thermostat::GetBlowerState() const { return previousState ? previousState->blowerState : -1; }

std::ostream& operator<<(std::ostream& os, const Thermostat& tstat) {
  using namespace std::chrono;
  if (tstat.previousState) os << *tstat.previousState << " ";
  os << "  Time since transition: "
     << duration_cast<seconds>(tstat.GetTimeSinceTransition()).count();
  return os;
}

CeilingFan::CeilingFan(CURL* curlInstance)
    : Fan(curlInstance), fanStateUpdatedSinceLastTransition(false) {}
CeilingFan::~CeilingFan() {}

bool CeilingFan::SetFanSpeed(const int speed) {
  using namespace std::chrono;
  const auto startTime(steady_clock::now());

  const std::string postData = "{\"fanSpeed\": " + std::to_string(speed) + "}";
  curl_easy_setopt(curlInstance, CURLOPT_POSTFIELDS, postData.c_str());
  auto result = doHttpRequest(curlInstance);
  const std::string fanURL = GetURL(curlInstance);
  const auto opTime(duration_cast<milliseconds>(steady_clock::now() - startTime));
  std::cout << "  Setting fan " << fanURL << " speed to: " << postData
            << " Return Code: " << result.first << " took: " << opTime.count() << "ms" << std::endl;
  syslog(result.first == 200 ? LOG_INFO : LOG_ERR,
         "Setting fan %s speed to: %d.  %ld : %s (%ld ms)", GetURL(curlInstance).c_str(), speed,
         result.first, result.first == 200 ? "" : result.second.c_str(), opTime.count());
#ifdef DEBUG
  std::cout << "Fan return code :" << result.first << std::endl << result.second << std::endl;
#endif
  return (result.first == 200);
}

int CeilingFan::GetFanSpeed() {
  curl_easy_setopt(curlInstance, CURLOPT_POSTFIELDS, "{\"queryDynamicShadowData\": 1}");
  auto fanQuery = doHttpRequest(curlInstance);
  if (fanQuery.first != 200) return -1;
  rapidjson::Document jsonDoc;
  jsonDoc.Parse(fanQuery.second.c_str());
#ifdef DEBUG
  writeJsonOut(jsonDoc);
#endif
  return jsonDoc["fanSpeed"].GetInt();
}

void CeilingFan::Reboot() {
  // Reboot commands don't get a response, instead they will timeout.  :/
  curl_easy_setopt(curlInstance, CURLOPT_POSTFIELDS, "{\"reboot\": 1}");
  doHttpRequest(curlInstance);
}

void CeilingFan::Update(const Thermostat& tstat) {
  if (tstat.StateChanged()) {
    fanStateUpdatedSinceLastTransition = false;
  } else if (!fanStateUpdatedSinceLastTransition &&
             tstat.GetTimeSinceTransition() >
                 (tstat.isFurnaceOn() ? k_ceilingFanOnDelay : k_ceilingFanOffDelay)) {
    fanStateUpdatedSinceLastTransition =
        SetFanSpeed(tstat.isFurnaceOn() ? k_heatOnFanSpeed : k_heatOffFanSpeed);
  }
}

void CeilingFan::Debug() {
  curl_easy_setopt(curlInstance, CURLOPT_POSTFIELDS, "{\"queryDynamicShadowData\": 1}");
  auto fanQuery = doHttpRequest(curlInstance);
  std::cout << "Fan query response for: " << GetURL(curlInstance) << " " << fanQuery.first
            << std::endl
            << fanQuery.second << std::endl
            << std::endl;
}

FurnaceBlower::FurnaceBlower(CURL* curlInstance) : Fan(curlInstance) {}
FurnaceBlower::~FurnaceBlower() {}
void FurnaceBlower::Update(const Thermostat& tstat) {
  const int currentBlowerState = tstat.GetBlowerState();
  if (!tstat.isFurnaceOn() &&
      (tstat.StateChanged() || tstat.GetTimeSinceTransition() < k_runBlowerFanAfterHeatOff)) {
    if (!latchedState && currentBlowerState != -1) {
      latchedState = currentBlowerState;
      std::cout << "Latched blower state to: " << currentBlowerState << std::endl;
    }
    if (currentBlowerState != BLOWER_ON) {
      SetBlowerState(BLOWER_ON);
    }
  } else if (latchedState) {
    if (latchedState == currentBlowerState) {
      latchedState.reset();
    } else {
      SetBlowerState(*latchedState);
    }
  }
}

void FurnaceBlower::Debug() { Thermostat(curlInstance).Debug(); }

bool FurnaceBlower::SetBlowerState(int newState) {
  using namespace std::chrono;
  const auto startTime(steady_clock::now());

  const std::string postData = "{\"fmode\": " + std::to_string(newState) + "}";
  curl_easy_setopt(curlInstance, CURLOPT_POSTFIELDS, postData.c_str());
  auto result = doHttpRequest(curlInstance);
  const auto opTime(duration_cast<milliseconds>(steady_clock::now() - startTime));
  std::cout << "  Set blower fan to: " << postData.c_str() << " Return code :" << result.first
            << " took: " << opTime.count() << "ms" << std::endl;
  syslog(result.first == 200 ? LOG_INFO : LOG_ERR, "Setting blower %s to: %d, response %s (%ld ms)",
         GetURL(curlInstance).c_str(), newState, result.second.c_str(), opTime.count());
  return (result.first == 200);
}
}  // namespace

int main(int argc, char* argv[]) {
  openlog("fancontrol", 0, LOG_USER);
  CurlObj tstatCurl("http://192.168.0.73/tstat");
  CurlObj fan1Curl("http://192.168.0.75/mf");
  CurlObj fan2Curl("http://192.168.0.76/mf");
  CurlObj fan3Curl("http://192.168.0.77/mf");

  std::vector<std::unique_ptr<Fan>> fans;
  fans.push_back(std::make_unique<CeilingFan>(fan1Curl()));
  fans.push_back(std::make_unique<CeilingFan>(fan2Curl()));
  fans.push_back(std::make_unique<CeilingFan>(fan3Curl()));
  fans.push_back(std::make_unique<FurnaceBlower>(tstatCurl()));

  using std::chrono::steady_clock;

  Thermostat tstat(tstatCurl());

  if (argc > 1 && std::string(argv[1]).rfind("-d", 0) == 0) {
    std::cout << "Fetching Debug data" << std::endl;
    for (auto& fan : fans) {
      fan->Debug();
    }
    return 0;
  }

#ifdef DEBUG
  tstat.Update();
  std::cout << CeilingFan(fan1Curl).GetFanSpeed() << std::endl;
#endif

  while (true) {
    const auto loopStartTime = steady_clock::now();

    if (tstat.Update()) {
      for (auto& fan : fans) {
        fan->Update(tstat);
      }
      std::cout << tstat << std::endl;
    }

    const auto loopExecTime = steady_clock::now() - loopStartTime;
    std::this_thread::sleep_for(k_thermostatPollFrequencySeconds - loopExecTime);
  }

  return 0;
}
