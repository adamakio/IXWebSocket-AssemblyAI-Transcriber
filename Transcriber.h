#ifndef Transcriber_H
#define Transcriber_H


#include "portaudio.h"
#include <nlohmann/json.hpp>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXNetSystem.h>

#include <cstdint>
#include <string>
#include <iostream>
#include <vector>
#include <deque>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <mutex>
#include <memory>

using Json = nlohmann::json;

static const std::string base64_chars =
"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
"abcdefghijklmnopqrstuvwxyz"
"0123456789+/";

std::string base64_encode(const std::vector<char>& buf) {
    std::string base64;
    int i = 0;
    int j = 0;
    unsigned char char_array_3[3];
    unsigned char char_array_4[4];

    for (size_t idx = 0; idx < buf.size(); ++idx) {
        char_array_3[i++] = buf[idx];
        if (i == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;

            for (i = 0; (i < 4); i++)
                base64 += base64_chars[char_array_4[i]];
            i = 0;
        }
    }

    if (i) {
        for (j = i; j < 3; j++)
            char_array_3[j] = '\0';

        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);

        for (j = 0; (j < i + 1); j++)
            base64 += base64_chars[char_array_4[j]];

        while ((i++ < 3))
            base64 += '=';
    }

    return base64;
}

class Transcriber
{
public:
    Transcriber(int sample_rate) 
        : m_sampleRate(sample_rate)
        , m_framesPerBuffer(static_cast<int>(sample_rate * 0.2f))
    {
        // WebSocket initialization
        ix::initNetSystem();

        // PortAudio initialization
        {
            m_audioErr = Pa_Initialize();
            if (m_audioErr != paNoError) {
                std::cout << "PortAudio error when initializing: " << Pa_GetErrorText(m_audioErr) << '\n';
                return;
            }

            m_audioErr = Pa_OpenDefaultStream(
                &m_audioStream,
                m_channels,
                0,
                m_format,
                m_sampleRate,
                m_framesPerBuffer,
                &Transcriber::pa_callback,
                this
            );
            if (m_audioErr != paNoError) {
                std::cout << "PortAudio error when initializing: " << Pa_GetErrorText(m_audioErr) << '\n';
                return;
            }
        }

        // JSON initialization
        m_audioJSON["audio_data"] = "";
    }

    ~Transcriber() {
        if (m_running)
            stop_transcription();
        if (m_audioStream)
            Pa_CloseStream(m_audioStream);
        Pa_Terminate();
        ix::uninitNetSystem();
    }

    void start_transcription() {
        std::lock_guard<std::mutex> lock(m_startStopMutex);
        if (m_running) {
            std::cout << "Transcription already started\n";
            return;
        }

        m_audioErr = Pa_StartStream(m_audioStream);
        if (m_audioErr != paNoError) {
            std::cout << "PortAudio error when starting stream: " << Pa_GetErrorText(m_audioErr) << '\n';
            Pa_CloseStream(m_audioStream);
            Pa_Terminate();
            m_audioStream = nullptr;
            return;
        }
        std::cout << "PortAudio stream started\n";

        // Set up WebSocket URL and headers
        std::string url = "wss://api.assemblyai.com/v2/realtime/ws?sample_rate=" + std::to_string(m_sampleRate);
        m_webSocket.setExtraHeaders({ {"Authorization", m_aaiAPItoken} });
        m_webSocket.setUrl(url);

        // Initialize WebSocket
        m_webSocket.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
            this->on_message(msg);
            });

        // Start WebSocket
        m_webSocket.start();
        m_webSocket.enableAutomaticReconnection();

        // Per message deflate connection is enabled by default. You can tweak its parameters or disable it
        m_webSocket.disablePerMessageDeflate();

        // Optional heart beat, sent every 20 seconds when there is not any traffic
        // to make sure that load balancers do not kill an idle connection.
        m_webSocket.setPingInterval(20);

        // Start the audio sending thread
        m_running = true;
        m_sendAudioThread = std::thread(&Transcriber::send_audio_data_thread, this);
    }

    void stop_transcription() {
        std::lock_guard<std::mutex> lock(m_startStopMutex);
        if (!m_running) {
            std::cout << "Transcription already stopped\n";
            return;
        }

        m_running = false;
        if (m_sendAudioThread.joinable()) {
            m_sendAudioThread.join();
        }

        // Stop portaudio stream
        m_audioErr = Pa_IsStreamActive(m_audioStream);
        if (m_audioErr == 1) {
            m_audioErr = Pa_StopStream(m_audioStream);
            if (m_audioErr != paNoError) {
                std::cout << "PortAudio error when stopping stream: " << Pa_GetErrorText(m_audioErr) << '\n';
            }
        }

        auto sendInfo = m_webSocket.send(m_terminateMsg);
        if (sendInfo.success) {
            std::cout << "Terminate message sent successfully\n";
        }
        else {
            std::cout << "Terminate message sending failed\n";
        }
        m_webSocket.stop();

    }

private:
    static int pa_callback(
        const void* inputBuffer,
        void* outputBuffer,
        unsigned long framesPerBuffer,
        const PaStreamCallbackTimeInfo* timeInfo,
        PaStreamCallbackFlags statusFlags,
        void* userData
    ) {
        auto* transcriber = static_cast<Transcriber*>(userData);
        return transcriber->on_audio_data(inputBuffer, framesPerBuffer);
    }

    int on_audio_data(const void* inputBuffer, unsigned long framesPerBuffer)
    {
        // Cast data passed through stream to our structure.
        const auto* in = static_cast<const int16_t*>(inputBuffer);
        m_audioDataBuffer.assign(reinterpret_cast<const char*>(in), reinterpret_cast<const char*>(in) + framesPerBuffer * m_channels * sizeof(int16_t)); // Copy the audio data into the buffer
        {
            // Enqueue the audio data
            std::lock_guard<std::mutex> lock(m_audioQueueMutex);
            m_audioQueue.push_back(m_audioDataBuffer);
        }
        return paContinue;
    }

    void send_audio_data_thread() {
        while (m_running) {
            std::vector<char> audio_data;
            {
                std::lock_guard<std::mutex> lock(m_audioQueueMutex);
                if (!m_audioQueue.empty()) {
                    audio_data = m_audioQueue.front();
                    m_audioQueue.pop_front();
                }
            }

            if (!audio_data.empty()) {
                // Encode and send the audio data
                m_audioJSON["audio_data"] = base64_encode(audio_data); // Implement base64_encode as needed
                m_webSocket.send(m_audioJSON.dump());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    void on_message(const ix::WebSocketMessagePtr& msg) 
    {
        switch (msg->type) {
        case ix::WebSocketMessageType::Message: {
            // Parse the JSON message
            Json json_msg = Json::parse(msg->str);

            if (json_msg.contains("error")) {
                std::cout << "Error from websocket: " << json_msg["error"] << '\n';
                return;
            }

            // Extract the message type
            std::string message_type = json_msg["message_type"];

            if (message_type == "PartialTranscript") {
                std::cout << "\r" << json_msg["text"] << std::flush; // Use carriage return to overwrite the previous line
            }
            else if (message_type == "FinalTranscript") {
                std::cout << '\r' << json_msg["text"] << std::endl;
            }
            else if (message_type == "SessionBegins") {
                std::cout << "Session started with ID: " << json_msg["session_id"] 
                    << " and expires at: " << json_msg["expires_at"] << '\n';
            }
            else if (message_type == "SessionTerminated") {
                std::cout << "Session terminated.";
            }
            else {
                std::cout << "Unknown message type: " << message_type;
            }
            return;
        }
        case ix::WebSocketMessageType::Open:
            std::cout << "WebSocket connection opened with message " << msg->str << '\n';

            std::cout << "Connection to " << msg->openInfo.uri
                << " opened with protocol: " << msg->openInfo.protocol
                << " and handshake headers:";
            for (auto it : msg->openInfo.headers) {
                std::cout << it.first << ": " << it.second << '\n';
            }
            return;
        case ix::WebSocketMessageType::Close:
            std::cout << "WebSocket connection closed with message " << msg->str << '\n';
            std::cout << "Closing connection because error code " 
                << msg->closeInfo.code
                << " : " << msg->closeInfo.reason 
                << "(remote ? " << msg->closeInfo.remote << ")\n";
            return;
        case ix::WebSocketMessageType::Error:
            std::cout << "WebSocket error with message " << msg->str;
            std::cout << "Error: " << msg->errorInfo.reason
                << "\n#retries: " << msg->errorInfo.retries
                << "\nWait time(ms): " << msg->errorInfo.wait_time
                << "\nHTTP Status: " << msg->errorInfo.http_status;
            return;
        }
    }

    ix::WebSocket m_webSocket;

    std::thread m_sendAudioThread;
    std::atomic<bool> m_running{ false };

    Json m_terminateJSON{ {"terminate_session", true} };
    std::string m_terminateMsg{ m_terminateJSON.dump() };

    std::vector<char> m_audioDataBuffer;
    Json m_audioJSON;

    std::deque<std::vector<char>> m_audioQueue;
    std::mutex m_audioQueueMutex;

    std::mutex m_startStopMutex;

    PaStream* m_audioStream{ nullptr };
    PaError m_audioErr{ paNoError };

    const std::string m_aaiAPItoken{ "7e4983bb8d1d47acb2dec97ee5e4c3ed" };
    const int m_sampleRate;
    const int m_framesPerBuffer;
    const PaSampleFormat m_format{ paInt16 };
    const int m_channels{ 1 };
};


#endif // Transcriber_H

