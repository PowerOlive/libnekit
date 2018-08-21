// MIT License

// Copyright (c) 2018 Zhuhao Wang

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "nekit/data_flow/tls_data_flow.h"

#include "nekit/utils/error.h"

#define NE_TLS_TUNNEL_CHECK_ERROR(tunnel__) assert(!tunnel__.Errored())

namespace nekit {
namespace data_flow {
TlsDataFlow::TlsDataFlow(std::shared_ptr<utils::Session> session,
                         std::shared_ptr<SSL_CTX> ctx,
                         std::unique_ptr<RemoteDataFlowInterface>&& data_flow)
    : session_{session},
      tunnel_{ctx, crypto::TlsTunnel::Mode::Client},
      data_flow_{std::move(data_flow)} {};

TlsDataFlow::~TlsDataFlow() {
  read_cancelable_.Cancel();
  write_cancelable_.Cancel();
  connect_cancelable_.Cancel();
  next_read_cancelable_.Cancel();
  next_write_cancelable_.Cancel();
}

utils::Cancelable TlsDataFlow::Read(utils::Buffer&& buffer,
                                    DataEventHandler handler) {
  (void)buffer;
  BOOST_ASSERT(!error_reported_);

  read_cancelable_ = utils::Cancelable();
  read_handler_ = handler;

  state_machine_.ReadBegin();
  Process();

  return read_cancelable_;
}

utils::Cancelable TlsDataFlow::Write(utils::Buffer&& buffer,
                                     EventHandler handler) {
  BOOST_ASSERT(!error_reported_);

  write_cancelable_ = utils::Cancelable();
  write_handler_ = handler;

  state_machine_.WriteBegin();

  tunnel_.WritePlainTextData(std::move(buffer));

  Process();

  return write_cancelable_;
}

utils::Cancelable TlsDataFlow::CloseWrite(EventHandler handler) {
  (void)handler;
  return write_cancelable_;
}

const data_flow::FlowStateMachine& TlsDataFlow::StateMachine() const {
  return state_machine_;
}

data_flow::DataFlowInterface* TlsDataFlow::NextHop() const {
  return data_flow_.get();
}

std::shared_ptr<utils::Endpoint> TlsDataFlow::ConnectingTo() {
  return connect_to_;
}

data_flow::DataType TlsDataFlow::FlowDataType() const {
  return data_flow::DataType::Stream;
}

std::shared_ptr<utils::Session> TlsDataFlow::Session() const {
  return session_;
}

utils::Runloop* TlsDataFlow::GetRunloop() { return data_flow_->GetRunloop(); }

utils::Cancelable TlsDataFlow::Connect(
    std::shared_ptr<utils::Endpoint> endpoint, EventHandler handler) {
  connect_cancelable_ = utils::Cancelable();
  connect_to_ = endpoint;
  tunnel_.SetDomain(endpoint->host());

  connect_handler_ = handler;
  state_machine_.ConnectBegin();
  (void)data_flow_->Connect(
      endpoint, [this, cancelable{connect_cancelable_}](utils::Error error) {
        if (cancelable.canceled()) {
          return;
        }

        if (error) {
          connect_handler_(error);
          return;
        }

        HandShake();
      });

  return connect_cancelable_;
}

RemoteDataFlowInterface* TlsDataFlow::NextRemoteHop() const {
  return static_cast<RemoteDataFlowInterface*>(data_flow_.get());
}

void TlsDataFlow::HandShake() {
  using namespace crypto;
  switch (tunnel_.HandShake()) {
    case TlsTunnel::HandShakeAction::Success: {
      if (utils::Buffer b = tunnel_.ReadCipherTextData()) {
        write_cancelable_ = data_flow_->Write(
            std::move(b),
            [this, cancelable{connect_cancelable_}](utils::Error error) {
              if (cancelable.canceled()) {
                return;
              }

              if (error) {
                state_machine_.Errored();
                connect_handler_(error);
                return;
              }
              HandShake();
              return;
            });
        return;
      }
      state_machine_.Connected();
      connect_handler_({});
      connect_handler_ = nullptr;
      return;
    }
    case TlsTunnel::HandShakeAction::WantIo: {
      if (utils::Buffer b = tunnel_.ReadCipherTextData()) {
        write_cancelable_ = data_flow_->Write(
            std::move(b),
            [this, cancelable{connect_cancelable_}](utils::Error error) {
              if (cancelable.canceled()) {
                return;
              }

              if (error) {
                state_machine_.Errored();
                connect_handler_(error);
                return;
              }
              HandShake();
              return;
            });
      } else {
        read_cancelable_ =
            data_flow_->Read(utils::Buffer(8192),
                             [this, cancelable{connect_cancelable_}](
                                 utils::Buffer&& buffer, utils::Error error) {
                               if (cancelable.canceled()) {
                                 return;
                               }

                               if (error) {
                                 state_machine_.Errored();
                                 connect_handler_(error);
                                 return;
                               }
                               tunnel_.WriteCipherTextData(std::move(buffer));
                               NE_TLS_TUNNEL_CHECK_ERROR(tunnel_);
                               HandShake();
                               return;
                             });
      }
      return;
    }
    case TlsTunnel::HandShakeAction::Error: {
      state_machine_.Errored();
      connect_handler_(utils::NEKitErrorCode::GeneralError);
      return;
    }
  }
}

void TlsDataFlow::Process() {
  if (error_reported_) {
    return;
  }

  if (pending_error_) {
    if (ReportError(pending_error_, true)) {
      error_reported_ = true;
    }
    return;
  }

  TryRead();
  TryWrite();
}

void TlsDataFlow::TryRead() {
  if (read_handler_) {
    if (tunnel_.HasPlainTextDataToRead()) {
      GetRunloop()->Post([this, buffer{tunnel_.ReadPlainTextData()},
                          cancelable{read_cancelable_}]() mutable {
        if (cancelable.canceled()) {
          return;
        }

        state_machine_.ReadEnd();

        auto handler = read_handler_;
        read_handler_ = nullptr;
        handler(std::move(buffer), {});
      });

      if (tunnel_.NeedCipherInput()) {
        TryReadNextHop();
      }

      return;
    } else {
      TryReadNextHop();
      return;
    }
  } else {
    if (tunnel_.NeedCipherInput()) {
      TryReadNextHop();
      return;
    }
  }
}

void TlsDataFlow::TryWrite() {
  if (tunnel_.FinishWritingCipherData() && write_handler_) {
    GetRunloop()->Post([this, cancelable{write_cancelable_}]() {
      if (cancelable.canceled()) {
        return;
      }

      state_machine_.WriteEnd();

      auto handler = write_handler_;
      write_handler_ = nullptr;
      handler({});
    });
    return;
  };

  if (!tunnel_.FinishWritingCipherData()) {
    TryWriteNextHop();
  }
}

void TlsDataFlow::TryReadNextHop() {
  if (data_flow_->StateMachine().IsReading()) {
    return;
  }

  utils::Buffer b(8192);
  next_read_cancelable_ = data_flow_->Read(
      std::move(b), [this, cancelable{read_cancelable_}](utils::Buffer&& buffer,
                                                         utils::Error error) {
        if (cancelable.canceled()) {
          return;
        }

        if (error) {
          if (!ReportError(error, true)) {
            pending_error_ = error;
          } else {
            error_reported_ = true;
          }
          return;
        }

        tunnel_.WriteCipherTextData(std::move(buffer));
        Process();
      });
}

void TlsDataFlow::TryWriteNextHop() {
  if (data_flow_->StateMachine().IsWriting()) {
    return;
  }

  if (tunnel_.FinishWritingCipherData()) {
    return;
  }

  next_write_cancelable_ = data_flow_->Write(
      tunnel_.ReadCipherTextData(),
      [this, cancelable{write_cancelable_}](utils::Error error) {
        if (cancelable.canceled()) {
          return;
        }

        if (error) {
          if (!ReportError(error, false)) {
            pending_error_ = error;
          } else {
            error_reported_ = true;
          }
          return;
        }

        Process();
      });
}

bool TlsDataFlow::ReportError(utils::Error error, bool try_read_first) {
  if (try_read_first ? ReadReportError(error) : WriteReportError(error)) {
    return true;
  }

  return try_read_first ? WriteReportError(error) : ReadReportError(error);
}

bool TlsDataFlow::ReadReportError(utils::Error error) {
  if (read_handler_) {
    // should we update error_reported here?
    auto handler = read_handler_;
    handler(utils::Buffer(), error);
    read_handler_ = nullptr;
    return true;
  }
  return false;
}

bool TlsDataFlow::WriteReportError(utils::Error error) {
  if (write_handler_) {
    auto handler = write_handler_;
    handler(error);
    write_handler_ = nullptr;
    return true;
  }

  return false;
}
}  // namespace data_flow
}  // namespace nekit
