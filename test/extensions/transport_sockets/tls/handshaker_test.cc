#include "envoy/network/transport_socket.h"
#include "envoy/ssl/handshaker.h"
#include "envoy/ssl/socket_state.h"

#include "extensions/transport_sockets/tls/handshaker_impl.h"

#include "test/extensions/transport_sockets/tls/ssl_certs_test.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "openssl/evp.h"
#include "openssl/hmac.h"
#include "openssl/ssl.h"

namespace Envoy {
namespace Extensions {
namespace TransportSockets {
namespace Tls {
namespace {

using ::testing::StrictMock;

// A callback shaped like pem_password_cb.
// See https://www.openssl.org/docs/man1.1.0/man3/pem_password_cb.html.
int PemPasswordCallback(char* buf, int buf_size, int /*rwflag*/, void* u) {
  if (u == nullptr) {
    return 0;
  }
  std::string passphrase = *reinterpret_cast<std::string*>(u);
  ASSERT(buf_size >= static_cast<int>(passphrase.size()));
  memcpy(buf, passphrase.data(), passphrase.size());
  return passphrase.size();
}

class MockHandshakerCallbacks : public Ssl::HandshakerCallbacks {
public:
  ~MockHandshakerCallbacks() override{};
  MOCK_METHOD(bssl::UniquePtr<SSL>, Handoff, (), (override));
  MOCK_METHOD(void, Handback, (bssl::UniquePtr<SSL>), (override));
  MOCK_METHOD(void, OnSuccessCb, (SSL*), (override));
  MOCK_METHOD(void, OnFailureCb, (), (override));
};

class HandshakerTest : public SslCertsTest {
protected:
  HandshakerTest()
      : dispatcher_(api_->allocateDispatcher("test_thread")), stream_info_(api_->timeSource()),
        client_ctx_(SSL_CTX_new(TLS_method())), server_ctx_(SSL_CTX_new(TLS_method())) {
    // Set up key and cert, initialize two SSL objects and a pair of BIOs for
    // handshaking.
    auto key = MakeKey();
    auto cert = MakeCert();
    auto chain = std::vector<CRYPTO_BUFFER*>{cert.get()};

    SSL_CTX_set_max_proto_version(server_ctx_.get(), TLS1_2_VERSION);

    server_ssl_ = bssl::UniquePtr<SSL>(SSL_new(server_ctx_.get()));
    SSL_set_accept_state(server_ssl_.get());
    ASSERT(
        SSL_set_chain_and_key(server_ssl_.get(), chain.data(), chain.size(), key.get(), nullptr));

    client_ssl_ = bssl::UniquePtr<SSL>(SSL_new(client_ctx_.get()));
    SSL_set_connect_state(client_ssl_.get());

    ASSERT(BIO_new_bio_pair(&client_bio_, kBufferLength, &server_bio_, kBufferLength));

    BIO_up_ref(client_bio_);
    BIO_up_ref(server_bio_);
    SSL_set0_rbio(client_ssl_.get(), client_bio_);
    SSL_set0_wbio(client_ssl_.get(), client_bio_);
    SSL_set0_rbio(server_ssl_.get(), server_bio_);
    SSL_set0_wbio(server_ssl_.get(), server_bio_);
  }

  // Read in key.pem and return a new private key.
  bssl::UniquePtr<EVP_PKEY> MakeKey() {
    std::string file = TestEnvironment::readFileToStringForTest(
        TestEnvironment::substitute("{{ test_tmpdir }}/unittestkey.pem"));
    std::string passphrase = "";
    bssl::UniquePtr<BIO> bio(BIO_new_mem_buf(file.data(), file.size()));

    bssl::UniquePtr<EVP_PKEY> key(EVP_PKEY_new());

    RSA* rsa = PEM_read_bio_RSAPrivateKey(bio.get(), nullptr, &PemPasswordCallback, &passphrase);
    ASSERT(rsa && EVP_PKEY_assign_RSA(key.get(), rsa));
    return key;
  }

  // Read in cert.pem and return a certificate.
  bssl::UniquePtr<CRYPTO_BUFFER> MakeCert() {
    std::string file = TestEnvironment::readFileToStringForTest(
        TestEnvironment::substitute("{{ test_tmpdir }}/unittestcert.pem"));
    bssl::UniquePtr<BIO> bio(BIO_new_mem_buf(file.data(), file.size()));

    uint8_t* data = nullptr;
    long len; // NOLINT (runtime/int)
    ASSERT(PEM_bytes_read_bio(&data, &len, nullptr, PEM_STRING_X509, bio.get(), nullptr, nullptr));
    bssl::UniquePtr<uint8_t> tmp(data); // Prevents memory leak.
    return bssl::UniquePtr<CRYPTO_BUFFER>(CRYPTO_BUFFER_new(data, len, nullptr));
  }

  const size_t kBufferLength{100};

  Event::DispatcherPtr dispatcher_;
  StreamInfo::StreamInfoImpl stream_info_;

  BIO *client_bio_, *server_bio_;
  bssl::UniquePtr<SSL_CTX> client_ctx_, server_ctx_;
  bssl::UniquePtr<SSL> client_ssl_, server_ssl_;
};

TEST_F(HandshakerTest, NormalOperation) {
  Network::MockTransportSocketCallbacks transport_socket_callbacks;
  transport_socket_callbacks.connection_.state_ = Network::Connection::State::Closed;
  EXPECT_CALL(transport_socket_callbacks, raiseEvent(Network::ConnectionEvent::Connected)).Times(1);
  EXPECT_CALL(transport_socket_callbacks, connection).Times(1);

  HandshakerImpl handshaker;
  handshaker.setTransportSocketCallbacks(transport_socket_callbacks);

  StrictMock<MockHandshakerCallbacks> callbacks;
  EXPECT_CALL(callbacks, OnSuccessCb).Times(1);

  auto socket_state = Ssl::SocketState::PreHandshake;
  auto post_io_action = Network::PostIoAction::KeepOpen; // default enum

  // Run the handshakes from the client and server until HandshakerImpl decides
  // we're done and returns PostIoAction::Close.
  while (post_io_action != Network::PostIoAction::Close) {
    SSL_do_handshake(client_ssl_.get());
    post_io_action = handshaker.doHandshake(socket_state, server_ssl_.get(), callbacks);
  }

  EXPECT_EQ(post_io_action, Network::PostIoAction::Close);
  // HandshakerImpl should have set |socket_state| accordingly.
  EXPECT_EQ(socket_state, Ssl::SocketState::HandshakeComplete);
}

// We induce some kind of BIO mismatch and force the SSL_do_handshake to
// return an error code without error handline, i.e. not SSL_ERROR_WANT_READ
// or _WRITE or _PRIVATE_KEY_OPERATION.
TEST_F(HandshakerTest, ErrorCbOnAbnormalOperation) {
  // We make a new BIO, set it as the rbio/wbio for the client SSL object, and
  // break the BIO pair connecting the two SSL objects. Now handshaking will
  // fail, likely with SSL_ERROR_SSL.
  BIO* bio = BIO_new(BIO_s_socket());
  SSL_set_bio(client_ssl_.get(), bio, bio);

  HandshakerImpl handshaker;

  StrictMock<Network::MockTransportSocketCallbacks> transport_socket_callbacks;
  handshaker.setTransportSocketCallbacks(transport_socket_callbacks);

  StrictMock<MockHandshakerCallbacks> callbacks;
  EXPECT_CALL(callbacks, OnFailureCb).Times(1);

  auto socket_state = Ssl::SocketState::PreHandshake;
  auto post_io_action = Network::PostIoAction::KeepOpen; // default enum

  while (post_io_action != Network::PostIoAction::Close) {
    SSL_do_handshake(client_ssl_.get());
    post_io_action = handshaker.doHandshake(socket_state, server_ssl_.get(), callbacks);
  }

  // In the error case, HandshakerImpl also closes the connection.
  EXPECT_EQ(post_io_action, Network::PostIoAction::Close);
}

} // namespace
} // namespace Tls
} // namespace TransportSockets
} // namespace Extensions
} // namespace Envoy
