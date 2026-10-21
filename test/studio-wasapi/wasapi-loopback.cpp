/* Private native Windows test recorder. Captures one explicit render endpoint;
 * does not change default devices, volumes or OBS settings. See README.md. */
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

static void Check(HRESULT hr, const char *operation)
{
	if (FAILED(hr)) {
		std::ostringstream error;
		error << operation << ": HRESULT 0x" << std::hex << uint32_t(hr);
		throw std::runtime_error(error.str());
	}
}

static std::string Utf8(const wchar_t *text)
{
	const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
	if (size <= 0) {
		throw std::runtime_error("WideCharToMultiByte failed");
	}
	std::string result(size, '\0');
	WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
	result.pop_back();
	return result;
}

static std::string Json(const std::string &text)
{
	std::ostringstream output;
	output << '"';
	for (unsigned char ch : text) {
		if (ch == '"' || ch == '\\') {
			output << '\\' << char(ch);
		} else if (ch < 32) {
			output << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(ch) << std::dec;
		} else {
			output << char(ch);
		}
	}
	output << '"';
	return output.str();
}

static std::string DeviceName(IMMDevice *device)
{
	ComPtr<IPropertyStore> store;
	Check(device->OpenPropertyStore(STGM_READ, &store), "OpenPropertyStore");
	PROPVARIANT value;
	PropVariantInit(&value);
	Check(store->GetValue(PKEY_Device_FriendlyName, &value), "GetValue FriendlyName");
	std::string result = value.vt == VT_LPWSTR ? Utf8(value.pwszVal) : "unknown";
	PropVariantClear(&value);
	return result;
}

static void WriteBytes(HANDLE file, const void *data, DWORD bytes)
{
	const auto *cursor = static_cast<const uint8_t *>(data);
	while (bytes) {
		DWORD written = 0;
		if (!WriteFile(file, cursor, bytes, &written, nullptr) || !written) {
			throw std::runtime_error("Writing WAV failed, Win32 error " + std::to_string(GetLastError()));
		}
		cursor += written;
		bytes -= written;
	}
}

static void Write32(HANDLE file, uint32_t value)
{
	WriteBytes(file, &value, sizeof(value));
}

static int Run(int argc, wchar_t **argv)
{
	bool list = false;
	std::wstring deviceId, filename;
	int seconds = 3;
	for (int i = 1; i < argc; ++i) {
		const std::wstring argument = argv[i];
		if (argument == L"--list-devices") {
			list = true;
		} else if (argument == L"--device-id" && i + 1 < argc) {
			deviceId = argv[++i];
		} else if (argument == L"--output" && i + 1 < argc) {
			filename = argv[++i];
		} else if (argument == L"--seconds" && i + 1 < argc) {
			seconds = std::stoi(argv[++i]);
		} else {
			throw std::runtime_error("Unknown or incomplete argument");
		}
	}
	ComPtr<IMMDeviceEnumerator> enumerator;
	Check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)),
	      "Create device enumerator");
	if (list) {
		ComPtr<IMMDeviceCollection> devices;
		Check(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices), "EnumAudioEndpoints");
		UINT count = 0;
		Check(devices->GetCount(&count), "GetCount");
		std::cout << "{\"render_endpoints\":[";
		for (UINT i = 0; i < count; ++i) {
			ComPtr<IMMDevice> device;
			Check(devices->Item(i, &device), "Item");
			LPWSTR id = nullptr;
			Check(device->GetId(&id), "GetId");
			const std::string encoded = Utf8(id);
			CoTaskMemFree(id);
			if (i) {
				std::cout << ',';
			}
			std::cout << "{\"id\":" << Json(encoded) << ",\"name\":" << Json(DeviceName(device.Get()))
				  << '}';
		}
		std::cout << "]}\n";
		return 0;
	}
	if (deviceId.empty() || filename.empty() || seconds < 1 || seconds > 30) {
		throw std::runtime_error("Use --device-id ID --output FILE.wav --seconds 1..30; or --list-devices");
	}
	const std::filesystem::path output(filename);
	ComPtr<IMMDevice> device;
	Check(enumerator->GetDevice(deviceId.c_str(), &device), "GetDevice");
	const auto name = DeviceName(device.Get());
	ComPtr<IAudioClient> client;
	Check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client), "Activate IAudioClient");
	WAVEFORMATEX *rawFormat = nullptr;
	Check(client->GetMixFormat(&rawFormat), "GetMixFormat");
	std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> format(rawFormat, CoTaskMemFree);
	const auto &wave = *format;
	if (!wave.nSamplesPerSec || !wave.nBlockAlign || wave.nChannels > 32) {
		throw std::runtime_error("Invalid device mix format");
	}
	Check(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 1000000, 0, format.get(),
				 nullptr),
	      "Initialize loopback");
	ComPtr<IAudioCaptureClient> capture;
	Check(client->GetService(IID_PPV_ARGS(&capture)), "GetService capture");
	const uint64_t wantedFrames = uint64_t(wave.nSamplesPerSec) * seconds;
	if (wantedFrames * wave.nBlockAlign > 256 * 1024 * 1024) {
		throw std::runtime_error("Capture exceeds fixture memory bound");
	}
	const HANDLE file =
		CreateFileW(output.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		throw std::runtime_error("Reserve new capture failed, Win32 error " + std::to_string(GetLastError()));
	}
	struct CloseFile {
		HANDLE file;
		~CloseFile() { CloseHandle(file); }
	} closeFile{file};
	const uint8_t silence = wave.wBitsPerSample == 8 ? 128 : 0;
	std::vector<uint8_t> samples(size_t(wantedFrames * wave.nBlockAlign), silence);
	uint64_t receivedFrames = 0, writtenFrames = 0, discontinuities = 0, timestampErrors = 0, packets = 0;
	Check(client->Start(), "Start");
	struct StopClient {
		IAudioClient *client;
		~StopClient()
		{
			if (client) {
				client->Stop();
			}
		}
	} stop{client.Get()};
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
	do {
		UINT32 available = 0;
		Check(capture->GetNextPacketSize(&available), "GetNextPacketSize");
		while (available && writtenFrames < wantedFrames && std::chrono::steady_clock::now() < deadline) {
			BYTE *data = nullptr;
			UINT32 frames = 0;
			DWORD flags = 0;
			const HRESULT received = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
			Check(received, "GetBuffer");
			if (received == AUDCLNT_S_BUFFER_EMPTY) {
				break;
			}
			const auto amount = std::min<uint64_t>(frames, wantedFrames - writtenFrames);
			if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data) {
				std::memcpy(samples.data() + writtenFrames * wave.nBlockAlign, data,
					    size_t(amount * wave.nBlockAlign));
			}
			writtenFrames += amount;
			receivedFrames += frames;
			++packets;
			discontinuities += !!(flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY);
			timestampErrors += !!(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR);
			Check(capture->ReleaseBuffer(frames), "ReleaseBuffer");
			Check(capture->GetNextPacketSize(&available), "GetNextPacketSize");
		}
		Sleep(2);
	} while (writtenFrames < wantedFrames && std::chrono::steady_clock::now() < deadline);
	Check(client->Stop(), "Stop");
	stop.client = nullptr;
	const uint32_t formatBytes = uint32_t(sizeof(WAVEFORMATEX) + wave.cbSize);
	const uint32_t dataBytes = uint32_t(samples.size());
	const uint8_t pad = 0;
	WriteBytes(file, "RIFF", 4);
	Write32(file, 4 + 8 + formatBytes + (formatBytes & 1) + 8 + dataBytes + (dataBytes & 1));
	WriteBytes(file, "WAVEfmt ", 8);
	Write32(file, formatBytes);
	WriteBytes(file, format.get(), formatBytes);
	if (formatBytes & 1) {
		WriteBytes(file, &pad, 1);
	}
	WriteBytes(file, "data", 4);
	Write32(file, dataBytes);
	WriteBytes(file, samples.data(), dataBytes);
	if (dataBytes & 1) {
		WriteBytes(file, &pad, 1);
	}
	if (!FlushFileBuffers(file)) {
		throw std::runtime_error("Flush WAV failed, Win32 error " + std::to_string(GetLastError()));
	}
	std::cout
		<< "{\"captured\":true,\"device_id\":" << Json(Utf8(deviceId.c_str()))
		<< ",\"device_name\":" << Json(name) << ",\"sample_rate\":" << wave.nSamplesPerSec
		<< ",\"channels\":" << wave.nChannels << ",\"format_tag\":" << wave.wFormatTag
		<< ",\"bits_per_sample\":" << wave.wBitsPerSample << ",\"requested_seconds\":" << seconds
		<< ",\"received_frames\":" << receivedFrames << ",\"written_frames\":" << writtenFrames
		<< ",\"padded_silent_frames\":" << wantedFrames - writtenFrames << ",\"packets\":" << packets
		<< ",\"discontinuities\":" << discontinuities << ",\"timestamp_errors\":" << timestampErrors
		<< ",\"scope\":\"WASAPI endpoint loopback, not acoustic measurement; silence padding is explicit\"}\n";
	return 0;
}

int wmain(int argc, wchar_t **argv)
{
	const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if (FAILED(init)) {
		std::cerr << "COM initialization failed\n";
		return 2;
	}
	int result = 2;
	try {
		result = Run(argc, argv);
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
	}
	CoUninitialize();
	return result;
}
