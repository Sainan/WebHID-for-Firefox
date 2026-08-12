#include <iostream>

#include <AtomicDeque.hpp>
#include <BufferWriter.hpp>
#include <CertStore.hpp>
#include <HttpRequest.hpp>
#include <hwHid.hpp>
#include <main.hpp>
#include <rsa.hpp>
#include <Server.hpp>
#include <ServerWebService.hpp>
#include <Socket.hpp>
#include <string.hpp>
#include <Thread.hpp>
#include <WebSocketMessage.hpp>

#define DEBUG false

using namespace soup;

// Based on https://source.chromium.org/chromium/chromium/src/+/main:services/device/public/cpp/hid/hid_blocklist.cc
[[nodiscard]] static bool hid_is_permitted(const hwHid& hid)
{
	return hid.usage_page != 0xF1D0 // FIDO page
		&& hid.vendor_id != 0x096E // Feitian Technologies (incl. KEY-ID & HyperFIDO)
		&& hid.vendor_id != 0x1050 // Yubico
		&& !(hid.vendor_id == 0x09C3 && hid.product_id == 0x0023) // HID Global BlueTrust Token
		&& !(hid.vendor_id == 0x10C4 && hid.product_id == 0x8ACF) // U2F Zero
		&& !(hid.vendor_id == 0x1209 && hid.product_id == 0x4321) // Mooltipass Mini-BLE
		&& !(hid.vendor_id == 0x1209 && hid.product_id == 0x4322) // Mooltipass Arduino sketch
		&& !(hid.vendor_id == 0x18D1 && hid.product_id == 0x5026) // Titan
		&& !(hid.vendor_id == 0x1A44 && hid.product_id == 0x00BB) // VASCO
		&& !(hid.vendor_id == 0x1D50 && hid.product_id == 0x60FC) // OnlyKey
		&& !(hid.vendor_id == 0x1E0D && hid.product_id == 0xF1AE) // Keydo AES
		&& !(hid.vendor_id == 0x1E0D && hid.product_id == 0xF1D0) // Neowave Keydo
		&& !(hid.vendor_id == 0x1EA8 && hid.product_id == 0xF025) // Thetis
		&& !(hid.vendor_id == 0x20A0 && hid.product_id == 0x4287) // Nitrokey
		&& !(hid.vendor_id == 0x24DC && hid.product_id == 0x0101) // JaCarta
		&& !(hid.vendor_id == 0x2581 && hid.product_id == 0xF1D0) // Happlink
		&& !(hid.vendor_id == 0x2ABE && hid.product_id == 0x1002) // Bluink
		&& !(hid.vendor_id == 0x2CCF && hid.product_id == 0x0880) // Feitian USB, HyperFIDO
		;
}

[[nodiscard]] static uint32_t hid_to_physical_hash(const hwHid& hid)
{
	uint32_t hash = soup::joaat::INITIAL;
	hash = soup::joaat::hashRange((const char*)&hid.vendor_id, sizeof(hid.vendor_id), hash);
	hash = soup::joaat::hashRange((const char*)&hid.product_id, sizeof(hid.product_id), hash);
	hash = soup::joaat::hash(hid.getManufacturerName(), hash);
	hash = soup::joaat::hash(hid.getProductName(), hash);
	hash = soup::joaat::hash(hid.getSerialNumber(), hash);
	return hash;
}

[[nodiscard]] static uint32_t hid_to_hash(const hwHid& hid)
{
	return soup::joaat::hash(hid.path);
}

struct ReceiveReportsTask;

struct ClientData
{
	std::vector<ReceiveReportsTask*> subscriptions;

	[[nodiscard]] ReceiveReportsTask* findSubscription(uint32_t hid_hash) const noexcept;
};

struct ReceiveReportsTask : public soup::Task
{
	SharedPtr<Worker> sock;
	hwHid hid;
	uint32_t hid_hash;
	Thread thrd;
	AtomicDeque<std::string> deque;

	ReceiveReportsTask(SharedPtr<Worker>&& _sock, hwHid&& hid, uint32_t hid_hash)
		: sock(std::move(_sock)), hid(std::move(hid)), hid_hash(hid_hash), thrd(&thrd_run, this)
	{
		static_cast<Socket&>(*sock).custom_data.getStructFromMap(ClientData).subscriptions.emplace_back(this);
	}

	static void thrd_run(Capture&& cap)
	{
		ReceiveReportsTask& task = cap.get<ReceiveReportsTask>();
		while (true)
		{
			const Buffer<>& report = task.hid.receiveReportWithReportId();
			SOUP_IF_UNLIKELY (report.empty())
			{
				//std::cout << "received empty report for " << task.hid_hash << std::endl;
				break;
			}
			//std::cout << "received report for " << task.hid_hash << std::endl;
			BufferWriter bw;
			uint8_t msgid = 1; bw.u8(msgid);
			bw.u32_be(task.hid_hash);
			bw.buf.append(report);
			task.deque.emplace_front(bw.buf.toString());
		}
		//std::cout << "thread stopping for " << task.hid_hash << std::endl;
	}

	void unsubscribe()
	{
		hid.cancelReceiveReport();
		removeFromClientSubscriptions();
		setWorkDone();
	}

	void removeFromClientSubscriptions() const
	{
		auto& subscriptions = static_cast<Socket&>(*sock).custom_data.getStructFromMap(ClientData).subscriptions;
		for (auto i = subscriptions.begin(); i != subscriptions.end(); ++i)
		{
			if (*i == this)
			{
				subscriptions.erase(i);
				break;
			}
		}
	}

	void onTick() final
	{
		SOUP_IF_UNLIKELY (static_cast<Socket&>(*sock).isWorkDoneOrClosed())
		{
			hid.cancelReceiveReport();
		}
		else
		{
			while (auto node = deque.pop_back())
			{
				//std::cout << "received report: " << string::bin2hex(*node) << std::endl;
				ServerWebService::wsSendBin(static_cast<Socket&>(*sock), std::move(*node));
			}
		}

		SOUP_IF_UNLIKELY (!thrd.isRunning())
		{
			std::string msg = "stopped:";
			msg.append(std::to_string(hid_hash));
			ServerWebService::wsSendText(static_cast<Socket&>(*sock), std::move(msg));

			removeFromClientSubscriptions();

			setWorkDone();
		}
	}

	int getSchedulingDisposition() const noexcept final
	{
		return HIGH_FREQUENCY;
	}
};

ReceiveReportsTask* ClientData::findSubscription(uint32_t hid_hash) const noexcept
{
	for (const auto& sub : subscriptions)
	{
		if (sub->hid_hash == hid_hash)
		{
			return sub;
		}
	}
	return nullptr;
}

struct ListDevicesTask : public soup::Task
{
	SharedPtr<Worker> sock;
	Thread thrd;
	std::vector<std::string> msgs;

	ListDevicesTask(SharedPtr<Worker>&& _sock)
		: sock(std::move(_sock)), thrd(&thrd_run, this)
	{
	}

	static void thrd_run(Capture&& cap)
	{
		ListDevicesTask& task = cap.get<ListDevicesTask>();
		for (const auto& hid : hwHid::getAll())
		{
			if (hid_is_permitted(hid))
			{
				std::string& msg = task.msgs.emplace_back("dev:");
				/*  [1] */ msg.append(std::to_string(hid_to_hash(hid))).push_back(':');
				/*  [2] */ msg.append(std::to_string(hid_to_physical_hash(hid))).push_back(':');
				/*  [3] */ msg.append(std::to_string(hid.vendor_id)).push_back(':');
				/*  [4] */ msg.append(std::to_string(hid.product_id)).push_back(':');
				/*  [5] */ msg.append(hid.getProductName()).push_back(':');
				/*  [6] */ msg.append(std::to_string(hid.usage)).push_back(':');
				/*  [7] */ msg.append(std::to_string(hid.usage_page)).push_back(':');
				/*  [8] */ msg.append(std::to_string(hid.input_report_byte_length)).push_back(':');
				/*  [9] */ msg.append(std::to_string(hid.output_report_byte_length)).push_back(':');
				/* [10] */ msg.append(std::to_string(hid.feature_report_byte_length)).push_back(':');
				std::vector<std::string> report_ids{};
				for (unsigned int i = 0; i != 0x100; ++i)
				{
					if (hid.hasReportId(i))
					{
						report_ids.emplace_back(std::to_string(i));
					}
				}
				/* [11] */ msg.append(string::join(report_ids, ','));
			}
		}
	}

	void onTick() final
	{
		if (!thrd.isRunning())
		{
			for (auto& msg : msgs)
			{
				ServerWebService::wsSendText(static_cast<Socket&>(*sock), std::move(msg));
			}
			ServerWebService::wsSendText(static_cast<Socket&>(*sock), "dev");
			setWorkDone();
		}
	}
};

int entry(std::vector<std::string>&& args, bool console)
{
	auto certstore = soup::make_shared<CertStore>();
	{
		X509Certchain certchain;
		SOUP_ASSERT(certchain.fromPem(R"(-----BEGIN CERTIFICATE-----
MIIGkTCCBPmgAwIBAgIQUEtIWNn+Hqae/3ukauzicDANBgkqhkiG9w0BAQsFADBg
MQswCQYDVQQGEwJHQjEYMBYGA1UEChMPU2VjdGlnbyBMaW1pdGVkMTcwNQYDVQQD
Ey5TZWN0aWdvIFB1YmxpYyBTZXJ2ZXIgQXV0aGVudGljYXRpb24gQ0EgRFYgUjM2
MB4XDTI2MDIwNjAwMDAwMFoXDTI3MDMwNjIzNTk1OVowGDEWMBQGA1UEAwwNKi5m
YWtldGxzLmNvbTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAKVZ217X
Jsx0+rT6wsgsuCj525k0/UX0SnRVeXO2tzqqs1FSiZm8wWU/WS++BURajCLKiSxV
yKqger1rsWoMzSmZAp2fR0RQLFSWq44ckElaADOfs7oY1FhhAoc6WvJ6PN67Xy8R
51ceSsC0S+nEozivkfuwlT8VZcvnG6DAo88BjMBZangSZCqwXuOg5mT3c33uoEVi
SCJF1K6WMm18frXeAvqjqePf/zIKJ8/9EOLP2HzBoQyzmO6iBtnxSeoDR30bB1v/
rsOr4qmBq2Sn/2ip2PS29b7H7ad7MTnq7NharN3cWv9MOsirDS2gJoyhMT2TIOJa
rKBW7eyvN1BzDukCAwEAAaOCAw0wggMJMB8GA1UdIwQYMBaAFGjAEhYYDq/O9oem
MlejRlFdywcnMB0GA1UdDgQWBBQntLhxiXiyg6/HZcRkNOc1fBkKRTAOBgNVHQ8B
Af8EBAMCBaAwDAYDVR0TAQH/BAIwADAdBgNVHSUEFjAUBggrBgEFBQcDAQYIKwYB
BQUHAwIwSQYDVR0gBEIwQDA0BgsrBgEEAbIxAQICBzAlMCMGCCsGAQUFBwIBFhdo
dHRwczovL3NlY3RpZ28uY29tL0NQUzAIBgZngQwBAgEwgYQGCCsGAQUFBwEBBHgw
djBPBggrBgEFBQcwAoZDaHR0cDovL2NydC5zZWN0aWdvLmNvbS9TZWN0aWdvUHVi
bGljU2VydmVyQXV0aGVudGljYXRpb25DQURWUjM2LmNydDAjBggrBgEFBQcwAYYX
aHR0cDovL29jc3Auc2VjdGlnby5jb20wJQYDVR0RBB4wHIINKi5mYWtldGxzLmNv
bYILZmFrZXRscy5jb20wggGPBgorBgEEAdZ5AgQCBIIBfwSCAXsBeQB2AByfaCzp
+vBFaVD4G5aKh93bMhDYTObIsuOCUkrEz1mfAAABnDKpVkcAAAQDAEcwRQIgDUXl
ZtUyLBTWMD0/2voREXisVxZ/RlzrUGyt6VzY1zYCIQDJaHxpQHAsmhOjwBOQiHB4
g56OLayj99u5sy1vWgUoMAB+AI7KRwus3mrzogawpHqEt0b+H8a/lT4l5ptO5AJI
88boAAABnDKpWUIACAAABQACZuGKBAMARzBFAiAIUB2BxBJ6Dyy28SjU3WGxgjRn
Q/mx9adHKnj6Qu1jJwIhAMBvJdat8/Q8PwL2iDgaZnYrWJtWpogwInOPsVFg8LvP
AH8AWW5sM4aUsllyolbIoOjdkEp26Ag92oc7AQg4KBQ87lkAAAGcMqlXGQAIAAAF
AAAixcYEAwBIMEYCIQDDmhW3ydLGu2itmHam7tPKcczItKVuUNy0khLGL7RLlQIh
AJhTdxM59yYtpqwKeD/ydyUSxOMrQLZAhlBQBS5o5p0zMA0GCSqGSIb3DQEBCwUA
A4IBgQBhpdKZbmHtmTSsp6CPiEWOsmFXQaKVb9NleehHSRWpUQuP06RSjegS4mCh
ZdseF5bVnmtNdzCEFg4NW/zgy7Rnha9OoY724jmO/mbCSQyPo+4dLupoNTQ1YFNM
9lx0SW/A1YMJ5h2MPdawMXogw/7ddLbNvnS47T/XQTiW4+Mn3b4oqjlkW7EIxecb
R55e9SajHLEG6UGxHFXpQteJ4NnXlsFY1EMBhG0X1CiGo8ggoe2jr6f1bHi/teoQ
emIc0X5TvXB6WNhpYHLqfT+9kkKy55spTQEeLx/aosT4jc9sqbauU14OCwgLOQAG
Eb/KQwNGD/TbV8ivMVXhLrOCbieY/NwdwXOILAIahvF6Id/BrkWyTZSDvncMictk
JzXWCORZrEdD5DClZ8YfHSNZCenDCUs9o2gMJgvVNa+ZpAu6SPLg60AdHO/g8xGz
hPr0BJO76lg/ryIejdgeVVejpIuUYJhyqD1IzbykYafE0h0kHnbUxjy/PUyp/gBa
BjSyToY=
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIIGTDCCBDSgAwIBAgIQOXpmzCdWNi4NqofKbqvjsTANBgkqhkiG9w0BAQwFADBf
MQswCQYDVQQGEwJHQjEYMBYGA1UEChMPU2VjdGlnbyBMaW1pdGVkMTYwNAYDVQQD
Ey1TZWN0aWdvIFB1YmxpYyBTZXJ2ZXIgQXV0aGVudGljYXRpb24gUm9vdCBSNDYw
HhcNMjEwMzIyMDAwMDAwWhcNMzYwMzIxMjM1OTU5WjBgMQswCQYDVQQGEwJHQjEY
MBYGA1UEChMPU2VjdGlnbyBMaW1pdGVkMTcwNQYDVQQDEy5TZWN0aWdvIFB1Ymxp
YyBTZXJ2ZXIgQXV0aGVudGljYXRpb24gQ0EgRFYgUjM2MIIBojANBgkqhkiG9w0B
AQEFAAOCAY8AMIIBigKCAYEAljZf2HIz7+SPUPQCQObZYcrxLTHYdf1ZtMRe7Yeq
RPSwygz16qJ9cAWtWNTcuICc++p8Dct7zNGxCpqmEtqifO7NvuB5dEVexXn9RFFH
12Hm+NtPRQgXIFjx6MSJcNWuVO3XGE57L1mHlcQYj+g4hny90aFh2SCZCDEVkAja
EMMfYPKuCjHuuF+bzHFb/9gV8P9+ekcHENF2nR1efGWSKwnfG5RawlkaQDpRtZTm
M64TIsv/r7cyFO4nSjs1jLdXYdz5q3a4L0NoabZfbdxVb+CUEHfB0bpulZQtH1Rv
38e/lIdP7OTTIlZh6OYL6NhxP8So0/sht/4J9mqIGxRFc0/pC8suja+wcIUna0HB
pXKfXTKpzgis+zmXDL06ASJf5E4A2/m+Hp6b84sfPAwQ766rI65mh50S0Di9E3Pn
2WcaJc+PILsBmYpgtmgWTR9eV9otfKRUBfzHUHcVgarub/XluEpRlTtZudU5xbFN
xx/DgMrXLUAPaI60fZ6wA+PTAgMBAAGjggGBMIIBfTAfBgNVHSMEGDAWgBRWc1hk
lfmSGrASKgRieaFAFYghSTAdBgNVHQ4EFgQUaMASFhgOr872h6YyV6NGUV3LBycw
DgYDVR0PAQH/BAQDAgGGMBIGA1UdEwEB/wQIMAYBAf8CAQAwHQYDVR0lBBYwFAYI
KwYBBQUHAwEGCCsGAQUFBwMCMBsGA1UdIAQUMBIwBgYEVR0gADAIBgZngQwBAgEw
VAYDVR0fBE0wSzBJoEegRYZDaHR0cDovL2NybC5zZWN0aWdvLmNvbS9TZWN0aWdv
UHVibGljU2VydmVyQXV0aGVudGljYXRpb25Sb290UjQ2LmNybDCBhAYIKwYBBQUH
AQEEeDB2ME8GCCsGAQUFBzAChkNodHRwOi8vY3J0LnNlY3RpZ28uY29tL1NlY3Rp
Z29QdWJsaWNTZXJ2ZXJBdXRoZW50aWNhdGlvblJvb3RSNDYucDdjMCMGCCsGAQUF
BzABhhdodHRwOi8vb2NzcC5zZWN0aWdvLmNvbTANBgkqhkiG9w0BAQwFAAOCAgEA
YtOC9Fy+TqECFw40IospI92kLGgoSZGPOSQXMBqmsGWZUQ7rux7cj1du6d9rD6C8
ze1B2eQjkrGkIL/OF1s7vSmgYVafsRoZd/IHUrkoQvX8FZwUsmPu7amgBfaY3g+d
q1x0jNGKb6I6Bzdl6LgMD9qxp+3i7GQOnd9J8LFSietY6Z4jUBzVoOoz8iAU84OF
h2HhAuiPw1ai0VnY38RTI+8kepGWVfGxfBWzwH9uIjeooIeaosVFvE8cmYUB4TSH
5dUyD0jHct2+8ceKEtIoFU/FfHq/mDaVnvcDCZXtIgitdMFQdMZaVehmObyhRdDD
4NQCs0gaI9AAgFj4L9QtkARzhQLNyRf87Kln+YU0lgCGr9HLg3rGO8q+Y4ppLsOd
unQZ6ZxPNGIfOApbPVf5hCe58EZwiWdHIMn9lPP6+F404y8NNugbQixBber+x536
WrZhFZLjEkhp7fFXf9r32rNPfb74X/U90Bdy4lzp3+X1ukh1BuMxA/EEhDoTOS3l
7ABvc7BYSQubQ2490OcdkIzUh3ZwDrakMVrbaTxUM2p24N6dB+ns2zptWCva6jzW
r8IWKIMxzxLPv5Kt3ePKcUdvkBU/smqujSczTzzSjIoR5QqQA6lN1ZRSnuHIWCvh
JEltkYnTAH41QJ6SAWO66GrrUESwN/cgZzL4JLEqz1Y=
-----END CERTIFICATE-----)"));
		certstore->add(
			std::move(certchain),
			RsaPrivateKey::fromPem(R"(-----BEGIN PRIVATE KEY-----
MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQClWdte1ybMdPq0
+sLILLgo+duZNP1F9Ep0VXlztrc6qrNRUomZvMFlP1kvvgVEWowiyoksVciqoHq9
a7FqDM0pmQKdn0dEUCxUlquOHJBJWgAzn7O6GNRYYQKHOlryejzeu18vEedXHkrA
tEvpxKM4r5H7sJU/FWXL5xugwKPPAYzAWWp4EmQqsF7joOZk93N97qBFYkgiRdSu
ljJtfH613gL6o6nj3/8yCifP/RDiz9h8waEMs5juogbZ8UnqA0d9Gwdb/67Dq+Kp
gatkp/9oqdj0tvW+x+2nezE56uzYWqzd3Fr/TDrIqw0toCaMoTE9kyDiWqygVu3s
rzdQcw7pAgMBAAECggEABIYqfoQuPXSbafF8/BdeJA/fCmh04BG8qkVu7OO0THSA
csKKfYR4uaWTsf7V286K7EZEncwMkR+SMCXWzTcjs2ZKeCfUHjo/1kInmDwvlfQy
kncFhrtYbwEbfbny5FTEHtd7VZk8rklnw/FC7PbafOnv2XY1xSu3lO5CNyIiMT8X
Nl6xKauCIyqEQyGHsls1x8xJAP/GHaG77QXFEe8yYfghTMENqLWERVsh3J2+lT+J
ZmOiZf43EIn8pdWYSr11p5PKZKBzTsvl8f2jge5PjW5RDkm+eEBe7m7M5/cpQyYY
q/u2NnGjXwVu20qPYd9o2Pjqe2flZpjGhQ7HraWzsQKBgQDX5L45041p2+TFB8Gd
kDBgcy8OtZoip8+y8oeS34AUzPigWvsPbFx11oWoyldULqY3+f+IZecMWOrmmPvC
G+iimM5BSh6PM2wZLpxqHTeipLpSijxYv/mAZWtpq8IXzIrB570zAvfwICHH+G2w
7dDa7i8qvNZt1FFJXpzMTCcZ2QKBgQDEEXYHtLeLg12FB4kBkhnFUl9fH8bYWAW6
1X7OtcxBe5sN3zqdCO7h+xL3BLRJ8ATFll2wjkxV8KIPF/fQiHN/CQjfx6B+oRWL
/7FprB2vO47NTYsnxBkfI4MRxjjKJswtjRRLtXdrxocKCMpbbp+xgHuUBPKB3O7D
+EQaZjLjkQKBgHN+15Uv65MaJAST5axZGsg+VopT9KVdiOt+e7DDCE4YaVm/0lep
7LzNiquFs7pBSoLfpyhc+4HLf77lUKnDLGzvukeLU1XgeI/wM5VBFCZaYaMOeMIx
0VQy7YeWbzs0HXcmdjz39ZPsXKYR7Pyd/QFiMrF2XHBdCS9rAg75HmHhAoGADrmo
/acq6cVx7p3EQmPwn4syMni2fGUP+Rozrd6len2IBaCwTD7xgfNTV035JFxsX1KN
ssbdIM8zvKJ2MCkvU2kgS344pYn/jkYj0uXkTNbSQMUEIHZv1JDcNSeH+Fa1mu4v
iggU8fk1ByHl4LwMLk6R+WnttKcdH4azxq/KPqECgYBYXcAvOwthbyfTrqqyN8rg
oCVaEvRLdgBFkTVuCPsg68TQSfuRwE/Re/KpoRB+GEEZtGYVAG3ZCe0UKwV/nI+h
I2i4AQcJP+2m/tJSg9VsSBp+D5suTJ//ugHQA+hLejbkWg1i1kH3JSp0c+BZEE+i
fPOzDget78P/d2IgzbaKEA==
-----END PRIVATE KEY-----)")
		);
	}

	Server serv;
	ServerWebService web_srv;
	web_srv.should_accept_websocket_connection = [](Socket& s, const HttpRequest& req, ServerWebService&)
	{
#if DEBUG || !SOUP_WINDOWS
		return true;
#else
		if (auto origin = req.findHeader("Origin"))
		{
			// Run MessageBoxA off-thread because otherwise explorer freezes if the user confirms the prompt via a keyboard press (enter or spacebar). Can't make this shit up.
			// MB_DEFAULT_DESKTOP_ONLY makes the MessageBox appear on top of other apps, including Firefox, which is good as otherwise the user might be unaware of this prompt.
			static bool res;
			Thread thrd([](Capture&& cap)
			{
				res = (MessageBoxA(0, cap.get<std::string>().c_str(), "WebHID for Firefox", MB_YESNO | MB_DEFAULT_DESKTOP_ONLY) == IDYES);
			}, std::string("Allow the page at " + *origin + " to access your HID devices?"));
			thrd.awaitCompletion();
			return res;
		}
		return false;
#endif
	};
	web_srv.on_websocket_connection_established = [](Socket& s, const HttpRequest& req, ServerWebService&)
	{
		/*if (req.path == "/r1")
		{
			s.custom_data.getStructFromMap(ClientData).supports_report_ids = true;
		}*/
		ServerWebService::wsSendText(s, "ver:0.2.5");
	};
	web_srv.on_websocket_message = [](WebSocketMessage& msg, Socket& s, ServerWebService&)
	{
		if (msg.is_text)
		{
			//std::cout << "Text message: " << msg.data << std::endl;
			if (msg.data == "list")
			{
				Scheduler::get()->add<ListDevicesTask>(Scheduler::get()->getShared(s));
			}
			else if (msg.data.substr(0, 4) == "open")
			{
				const uint32_t hid_hash = std::strtoul(msg.data.c_str() + 4, nullptr, 10);
				if (s.custom_data.getStructFromMap(ClientData).findSubscription(hid_hash) == nullptr)
				{
					for (auto& hid : hwHid::getAll())
					{
						if (hid_to_hash(hid) == hid_hash && hid_is_permitted(hid))
						{
							Scheduler::get()->add<ReceiveReportsTask>(Scheduler::get()->getShared(s), std::move(hid), hid_hash);
							break;
						}
					}
				}
			}
			else if (msg.data.substr(0, 4) == "clse")
			{
				const uint32_t hid_hash = std::strtoul(msg.data.c_str() + 4, nullptr, 10);
				if (auto sub = s.custom_data.getStructFromMap(ClientData).findSubscription(hid_hash))
				{
					sub->unsubscribe();
				}
			}
		}
		else
		{
			//std::cout << "Binary message: " << string::bin2hex(msg.data) << std::endl;
			MemoryRefReader r(msg.data);
			uint8_t msgid; r.u8(msgid);
			if (msgid == 0)
			{
				if (msg.data.size() >= 5)
				{
					uint32_t hid_hash; r.u32_be(hid_hash);
					for (auto& hid : hwHid::getAll())
					{
						if (hid_to_hash(hid) == hid_hash && hid_is_permitted(hid))
						{
							Buffer data;
							data.append(msg.data.data() + 5, msg.data.size() - 5);
							//std::cout << "sending report: " << string::bin2hex(data.toString()) << std::endl;
							hid.sendReport(std::move(data));
							break;
						}
					}
				}
			}
			else if (msgid == 1)
			{
				if (msg.data.size() >= 5)
				{
					uint32_t hid_hash; r.u32_be(hid_hash);
					for (auto& hid : hwHid::getAll())
					{
						if (hid_to_hash(hid) == hid_hash && hid_is_permitted(hid))
						{
							Buffer data;
							data.append(msg.data.data() + 5, msg.data.size() - 5);
							//std::cout << "sending feature report: " << string::bin2hex(data.toString()) << std::endl;
							hid.sendFeatureReport(std::move(data));
							break;
						}
					}
				}
			}
			else if (msgid == 2)
			{
				if (msg.data.size() >= 5)
				{
					uint32_t hid_hash; r.u32_be(hid_hash);
					for (auto& hid : hwHid::getAll())
					{
						if (hid_to_hash(hid) == hid_hash && hid_is_permitted(hid))
						{
							uint8_t reportid = msg.data.data()[5];
							Buffer report;
							report.push_back(reportid);
							// std::cout << "receiving feature report: " << string::bin2hex(report.toString()) << std::endl;
							try
							{
								hid.receiveFeatureReport(report);
							}
							catch (std::exception &)
							{
							}
							// std::cout << "receiving feature report: " << string::bin2hex(report.toString()) << std::endl;
							BufferWriter bw;
							uint8_t msgid = 2; bw.u8(msgid);
							bw.u32_be(hid_hash);
							bw.u8(reportid);
							bw.buf.append(report);
							ServerWebService::wsSendBin(s, bw.buf.toString());
							break;
						}
					}
				}
			}
		}
	};
	if (!serv.bindCrypto(33881, &web_srv, std::move(certstore)))
	{
#if DEBUG || !SOUP_WINDOWS
		std::cout << "Failed to bind to port 33881." << std::endl;
#else
		MessageBoxA(0, "Failed to bind to port 33881.", "WebHID for Firefox", MB_ICONERROR);
#endif
		return 1;
	}
	std::cout << "Listening on port 33881." << std::endl;
	serv.run();
	return 0;
}

#if DEBUG
SOUP_MAIN_CLI(entry);
#else
SOUP_MAIN_GUI(entry);
#endif
