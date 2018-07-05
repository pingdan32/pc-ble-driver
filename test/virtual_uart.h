#ifndef VIRTUAL_UART_HPP__
#define VIRTUAL_UART_HPP__

#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <algorithm>

class VirtualUart : public Transport
{
private:
    std::string name;
    bool isOpen;
    VirtualUart* peer;

    std::mutex outDataMutex;
    std::vector<std::vector<uint8_t>> outData;
    std::condition_variable outDataAvailable;
    std::thread outDataThread;

    std::mutex inDataMutex;
    std::vector<std::vector<uint8_t>> inData;
    std::condition_variable inDataAvailable;
    std::thread inDataThread;

    control_pkt_type stopAtPktType;

    bool stoppedProcessing;

public:
    VirtualUart() = delete;

    VirtualUart(const std::string &name) :
        Transport(), name(name), isOpen(false), peer(nullptr), stopAtPktType(CONTROL_PKT_LAST), stoppedProcessing(false)
    {
    }

    void stopAt(control_pkt_type stopAtPktType_)
    {
        stopAtPktType = stopAtPktType_;
    }

    uint32_t open(status_cb_t status_callback, data_cb_t data_callback, log_cb_t log_callback) override
    {
        Transport::open(status_callback, data_callback, log_callback);

        if (peer == nullptr)
        {
            DEBUG("Peer port must be specified before calling open.");
            return NRF_ERROR_INTERNAL;
        }

        isOpen = true;

        outDataThread = std::thread([this]()
        {
            while (isOpen && stoppedProcessing == false)
            {
                std::unique_lock<std::mutex> lock(outDataMutex);

                if (outData.size() > 0)
                {
                    std::for_each(outData.begin(), outData.end(), [&](std::vector<uint8_t> data)
                    {
                        // TODO: do a proper SLIP decoding later on in case header hits SLIP encoding rules
                        if (H5Transport::isResetPacket(data, 2))
                        {
                            std::stringstream logEntry;
                            logEntry << "[" << name << "]" << " Requested to send RESET, ignoring since a reset does not make sense in this case.";
                            DEBUG(logEntry.str());
                        }
                        else
                        {
                            try
                            {
                                if (peer->isOpen)
                                {
                                    peer->injectInData(data);
                                }
                                else
                                {
                                    // TODO: report error back
                                }
                            }
                            catch (std::exception &e)
                            {
                                std::stringstream logEntry;
                                logEntry << "[" << name << "] " << "error sending " << e.what();
                                DEBUG(logEntry.str());
                            }
                        }
                    });
                    outData.erase(outData.begin(), outData.end());
                }

                outDataAvailable.wait(lock, [&] { return !(isOpen == true && outData.size() == 0 && stoppedProcessing == false); });
            }
        });

        inDataThread = std::thread([this]()
        {
            while (isOpen)
            {
                std::unique_lock<std::mutex> lock(inDataMutex);

                if (inData.size() > 0)
                {
                    std::for_each(inData.begin(), inData.end(), [&](std::vector<uint8_t> data)
                    {
                        // TODO: do a proper SLIP decoding later on in case header hits SLIP encoding rules

                        if (H5Transport::isResetPacket(data, 2))
                        {
                            std::stringstream logEntry;
                            logEntry << "[" << name << "] Received RESET, ignoring";
                            DEBUG(logEntry.str());
                        }
                        else if (H5Transport::isSyncPacket(data, 5) && stopAtPktType <= CONTROL_PKT_SYNC)
                        {
                            std::stringstream logEntry;
                            logEntry << "[" << name << "] Received SYNC ignored.";
                            DEBUG(logEntry.str());
                            stoppedProcessing = true;
                            outDataAvailable.notify_all();
                        }
                        else if (H5Transport::isSyncResponsePacket(data, 5) && stopAtPktType <= CONTROL_PKT_SYNC_RESPONSE)
                        {
                            std::stringstream logEntry;
                            logEntry << "[" << name << "] Received SYNC RESPONSE ignored.";
                            DEBUG(logEntry.str());
                            stoppedProcessing = true;
                            outDataAvailable.notify_all();
                        }
                        else if (H5Transport::isSyncConfigPacket(data, 5) && stopAtPktType <= CONTROL_PKT_SYNC_CONFIG)
                        {
                            std::stringstream logEntry;
                            logEntry << "[" << name << "] Received SYNC CONFIG ignored.";
                            DEBUG(logEntry.str());
                            stoppedProcessing = true;
                            outDataAvailable.notify_all();
                        }
                        else if (H5Transport::isSyncConfigResponsePacket(data, 5) && stopAtPktType <= CONTROL_PKT_SYNC_CONFIG_RESPONSE)
                        {
                            std::stringstream logEntry;
                            logEntry << "[" << name << "] Received SYNC CONFIG RESPONSE ignored.";
                            DEBUG(logEntry.str());
                            stoppedProcessing = true;
                            outDataAvailable.notify_all();
                        }
                        else
                        {
                            try
                            {
                                if (upperDataCallback != nullptr)
                                {
                                    upperDataCallback(data.data(), data.size());
                                }
                            }
                            catch (std::exception &e)
                            {
                                std::stringstream logEntry;
                                logEntry << "[" << name << "]" << name << "] error calling data callback: " << e.what();
                                DEBUG(logEntry.str());
                            }
                        }
                    });
                    inData.erase(inData.begin(), inData.end());
                }

                inDataAvailable.wait(lock, [&] { return !(isOpen == true && inData.size() == 0); });
            }
        });

        return NRF_SUCCESS; // TODO: take into account other return codes
    }

    uint32_t close() override
    {
        if (!isOpen) return NRF_ERROR_INTERNAL;

        isOpen = false;
        inDataAvailable.notify_all();
        outDataAvailable.notify_all();

        if (outDataThread.joinable())
        {
            outDataThread.join();
        }

        if (inDataThread.joinable())
        {
            inDataThread.join();
        }

        if (upperLogCallback != nullptr)
        {
            std::stringstream message;
            message << "serial port " << name << " closed.";
            upperLogCallback(SD_RPC_LOG_INFO, message.str());
        }

        return NRF_SUCCESS; // TODO: take into account other return codes
    }

    uint32_t send(const std::vector<uint8_t>& data) override
    {
        std::unique_lock<std::mutex> lock(outDataMutex);

        if (!isOpen)
        {
            return NRF_ERROR_INTERNAL;
        }

        outData.push_back(data);
        lock.unlock();
        outDataAvailable.notify_all();

        return NRF_SUCCESS; // TODO: take into account other states
    }

    void setPeer(VirtualUart* connectingPeer)
    {
        peer = connectingPeer;
    }

    // Used by peer to inject data into its incoming data pipe
    void injectInData(const std::vector<uint8_t> data)
    {
        std::unique_lock<std::mutex> lock(inDataMutex);
        inData.push_back(data);
        lock.unlock();
        inDataAvailable.notify_all();
    }

    ~VirtualUart() override
    {
        close();
    }
};

#endif // VIRTUAL_UART_HPP__