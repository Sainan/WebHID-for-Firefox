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
	bool supports_report_ids = false;

	[[nodiscard]] ReceiveReportsTask* findSubscription(uint32_t hid_hash) const noexcept;
};

struct ReceiveReportsTask : public soup::Task
{
	SharedPtr<Worker> sock;
	hwHid hid;
	uint32_t hid_hash;
	Thread thrd;
	AtomicDeque<std::string> deque;
	bool report_ids;

	ReceiveReportsTask(SharedPtr<Worker>&& _sock, hwHid&& hid, uint32_t hid_hash)
		: sock(std::move(_sock)), hid(std::move(hid)), hid_hash(hid_hash), thrd(&thrd_run, this), report_ids(static_cast<Socket&>(*sock).custom_data.getStructFromMap(ClientData).supports_report_ids)
	{
		static_cast<Socket&>(*sock).custom_data.getStructFromMap(ClientData).subscriptions.emplace_back(this);
	}

	static void thrd_run(Capture&& cap)
	{
		ReceiveReportsTask& task = cap.get<ReceiveReportsTask>();
		while (true)
		{
			const Buffer& report = (task.report_ids ? task.hid.receiveReportWithReportId() : task.hid.receiveReport());
			SOUP_IF_UNLIKELY (report.empty())
			{
				//std::cout << "received empty report for " << task.hid_hash << std::endl;
				break;
			}
			//std::cout << "received report for " << task.hid_hash << std::endl;
			BufferWriter bw;
			uint8_t msgid = (task.report_ids ? 1 : 0); bw.u8(msgid);
			bw.u32_be(task.hid_hash);
			bw.buf.append(report);
			task.deque.emplace_back(bw.buf.toString());
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
			while (auto node = deque.pop_front())
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

int entry(std::vector<std::string>&& args, bool console)
{
	auto certstore = soup::make_shared<CertStore>();
	{
		X509Certchain certchain;
		SOUP_ASSERT(certchain.fromPem(R"(-----BEGIN CERTIFICATE-----
MIIGLjCCBRagAwIBAgIRAPeLmReXnv+ALT/3Tm2Vts4wDQYJKoZIhvcNAQELBQAw
gY8xCzAJBgNVBAYTAkdCMRswGQYDVQQIExJHcmVhdGVyIE1hbmNoZXN0ZXIxEDAO
BgNVBAcTB1NhbGZvcmQxGDAWBgNVBAoTD1NlY3RpZ28gTGltaXRlZDE3MDUGA1UE
AxMuU2VjdGlnbyBSU0EgRG9tYWluIFZhbGlkYXRpb24gU2VjdXJlIFNlcnZlciBD
QTAeFw0yNDA0MTUwMDAwMDBaFw0yNTA0MTUyMzU5NTlaMBcxFTATBgNVBAMMDCou
cDJwdGxzLmNvbTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAKoxU6lW
K5iAXZfLrKOY5lcy7z+mML2cYZkW0XXJeC6jYDyYSGAPJogeIgd3JsJWjZvHxnj7
8KJGjO5j8B8kz4CVcV6aEx4ExJvtFUSzkgXHhlvSo2p0TTtWxC+ib3vWv+5kBSzb
4mdKKHiaz9shcLNKB77305xSBnKjAPGElgaZRwjwMqUSbPyjx4KrehyPQZDOU0aR
TKUbQNDbKYbeEmmUku0FTpao35GNsJrwzKKFIgzWAGKY+QiywIMeOGf0dTqX60GQ
MeXkKbueibuFKA12foV8RGojdT+bPIdRQyyEyntUkbu+UMknJ9bsPbKTEyQgv5nY
62O+A2lYG89Ub7MCAwEAAaOCAvowggL2MB8GA1UdIwQYMBaAFI2MXsRUrYrhd+mb
+ZsF4bgBjWHhMB0GA1UdDgQWBBQgFEQlEKO9vXkpBU7pQjbMU8MZvTAOBgNVHQ8B
Af8EBAMCBaAwDAYDVR0TAQH/BAIwADAdBgNVHSUEFjAUBggrBgEFBQcDAQYIKwYB
BQUHAwIwSQYDVR0gBEIwQDA0BgsrBgEEAbIxAQICBzAlMCMGCCsGAQUFBwIBFhdo
dHRwczovL3NlY3RpZ28uY29tL0NQUzAIBgZngQwBAgEwgYQGCCsGAQUFBwEBBHgw
djBPBggrBgEFBQcwAoZDaHR0cDovL2NydC5zZWN0aWdvLmNvbS9TZWN0aWdvUlNB
RG9tYWluVmFsaWRhdGlvblNlY3VyZVNlcnZlckNBLmNydDAjBggrBgEFBQcwAYYX
aHR0cDovL29jc3Auc2VjdGlnby5jb20wIwYDVR0RBBwwGoIMKi5wMnB0bHMuY29t
ggpwMnB0bHMuY29tMIIBfgYKKwYBBAHWeQIEAgSCAW4EggFqAWgAdgDPEVbu1S58
r/OHW9lpLpvpGnFnSrAX7KwB0lt3zsw7CAAAAY7jjWjnAAAEAwBHMEUCIQD/BajQ
AYjbiSmZZaTZ1j2miDHS4onTeIwMA5/jeAYzLgIgTAoSaQnX6Niyld5gmysgfkRC
zkiI/WwEJUxmI+R3Ll4AdwCi4wrkRe+9rZt+OO1HZ3dT14JbhJTXK14bLMS5UKRH
5wAAAY7jjWiVAAAEAwBIMEYCIQC1tH+VO0bRco4oSYvfsPaJDbLoJ2vfqSrCjtqu
nLavHwIhANuDbW4fRFA/myvN7mrLm3VLHI63RTl/gnzNqxodfB5oAHUATnWjJ1ya
EMM4W2zU3z9S6x3w4I4bjWnAsfpksWKaOd8AAAGO441ojgAABAMARjBEAiAzv6zf
dPxtnecz30Rb63+UiyvT2SdmdTTP+ap3r1rpCgIgX5z8mLnJJ3WL0LIB5NRC9qPn
/t324TkyWDHKgMPom2gwDQYJKoZIhvcNAQELBQADggEBAH7mgrQLmTkMs6/F/RoE
nsHQ9ddsDAA+Fs04alH8D8kuuXSsUWhaf0OYfBHLtOZ238qfigLxXZ6oGj9qNQ0I
hMP56sjEqd2IF2Vfi/qV3igLuJcICWnqqKIegCcS4fmy90NwYVtp2Z/7ovUa8aY/
yKGoXTfmDQwuyaH88j14Ft95lmvOJ4VPheGmSotZOaIkp1os/wPIoQAmWoecj173
jnLQ6O5/IZC4s/xKLKVt+vW+nmyR5U7VjUqAFN8eBHgdGWRcAiEaTRLBZMwWYP2D
XPFWmwT8vkvvK0WagFYOoITH9Zu13dHHzReIEyBhCDXWYyfib8i3K+acXidmi7Lu
fAw=
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIIGEzCCA/ugAwIBAgIQfVtRJrR2uhHbdBYLvFMNpzANBgkqhkiG9w0BAQwFADCB
iDELMAkGA1UEBhMCVVMxEzARBgNVBAgTCk5ldyBKZXJzZXkxFDASBgNVBAcTC0pl
cnNleSBDaXR5MR4wHAYDVQQKExVUaGUgVVNFUlRSVVNUIE5ldHdvcmsxLjAsBgNV
BAMTJVVTRVJUcnVzdCBSU0EgQ2VydGlmaWNhdGlvbiBBdXRob3JpdHkwHhcNMTgx
MTAyMDAwMDAwWhcNMzAxMjMxMjM1OTU5WjCBjzELMAkGA1UEBhMCR0IxGzAZBgNV
BAgTEkdyZWF0ZXIgTWFuY2hlc3RlcjEQMA4GA1UEBxMHU2FsZm9yZDEYMBYGA1UE
ChMPU2VjdGlnbyBMaW1pdGVkMTcwNQYDVQQDEy5TZWN0aWdvIFJTQSBEb21haW4g
VmFsaWRhdGlvbiBTZWN1cmUgU2VydmVyIENBMIIBIjANBgkqhkiG9w0BAQEFAAOC
AQ8AMIIBCgKCAQEA1nMz1tc8INAA0hdFuNY+B6I/x0HuMjDJsGz99J/LEpgPLT+N
TQEMgg8Xf2Iu6bhIefsWg06t1zIlk7cHv7lQP6lMw0Aq6Tn/2YHKHxYyQdqAJrkj
eocgHuP/IJo8lURvh3UGkEC0MpMWCRAIIz7S3YcPb11RFGoKacVPAXJpz9OTTG0E
oKMbgn6xmrntxZ7FN3ifmgg0+1YuWMQJDgZkW7w33PGfKGioVrCSo1yfu4iYCBsk
Haswha6vsC6eep3BwEIc4gLw6uBK0u+QDrTBQBbwb4VCSmT3pDCg/r8uoydajotY
uK3DGReEY+1vVv2Dy2A0xHS+5p3b4eTlygxfFQIDAQABo4IBbjCCAWowHwYDVR0j
BBgwFoAUU3m/WqorSs9UgOHYm8Cd8rIDZsswHQYDVR0OBBYEFI2MXsRUrYrhd+mb
+ZsF4bgBjWHhMA4GA1UdDwEB/wQEAwIBhjASBgNVHRMBAf8ECDAGAQH/AgEAMB0G
A1UdJQQWMBQGCCsGAQUFBwMBBggrBgEFBQcDAjAbBgNVHSAEFDASMAYGBFUdIAAw
CAYGZ4EMAQIBMFAGA1UdHwRJMEcwRaBDoEGGP2h0dHA6Ly9jcmwudXNlcnRydXN0
LmNvbS9VU0VSVHJ1c3RSU0FDZXJ0aWZpY2F0aW9uQXV0aG9yaXR5LmNybDB2Bggr
BgEFBQcBAQRqMGgwPwYIKwYBBQUHMAKGM2h0dHA6Ly9jcnQudXNlcnRydXN0LmNv
bS9VU0VSVHJ1c3RSU0FBZGRUcnVzdENBLmNydDAlBggrBgEFBQcwAYYZaHR0cDov
L29jc3AudXNlcnRydXN0LmNvbTANBgkqhkiG9w0BAQwFAAOCAgEAMr9hvQ5Iw0/H
ukdN+Jx4GQHcEx2Ab/zDcLRSmjEzmldS+zGea6TvVKqJjUAXaPgREHzSyrHxVYbH
7rM2kYb2OVG/Rr8PoLq0935JxCo2F57kaDl6r5ROVm+yezu/Coa9zcV3HAO4OLGi
H19+24rcRki2aArPsrW04jTkZ6k4Zgle0rj8nSg6F0AnwnJOKf0hPHzPE/uWLMUx
RP0T7dWbqWlod3zu4f+k+TY4CFM5ooQ0nBnzvg6s1SQ36yOoeNDT5++SR2RiOSLv
xvcRviKFxmZEJCaOEDKNyJOuB56DPi/Z+fVGjmO+wea03KbNIaiGCpXZLoUmGv38
sbZXQm2V0TP2ORQGgkE49Y9Y3IBbpNV9lXj9p5v//cWoaasm56ekBYdbqbe4oyAL
l6lFhd2zi+WJN44pDfwGF/Y4QA5C5BIG+3vzxhFoYt/jmPQT2BVPi7Fp2RBgvGQq
6jG35LWjOhSbJuMLe/0CjraZwTiXWTb2qHSihrZe68Zk6s+go/lunrotEbaGmAhY
LcmsJWTyXnW0OMGuf1pGg+pRyrbxmRE1a6Vqe8YAsOf4vmSyrcjC8azjUeqkk+B5
yOGBQMkKW+ESPMFgKuOXwIlCypTPRpgSabuY0MLTDXJLR27lk8QyKGOHQ+SwMj4K
00u/I5sUKUErmgQfky3xxzlIPK1aEn8=
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIIFgTCCBGmgAwIBAgIQOXJEOvkit1HX02wQ3TE1lTANBgkqhkiG9w0BAQwFADB7
MQswCQYDVQQGEwJHQjEbMBkGA1UECAwSR3JlYXRlciBNYW5jaGVzdGVyMRAwDgYD
VQQHDAdTYWxmb3JkMRowGAYDVQQKDBFDb21vZG8gQ0EgTGltaXRlZDEhMB8GA1UE
AwwYQUFBIENlcnRpZmljYXRlIFNlcnZpY2VzMB4XDTE5MDMxMjAwMDAwMFoXDTI4
MTIzMTIzNTk1OVowgYgxCzAJBgNVBAYTAlVTMRMwEQYDVQQIEwpOZXcgSmVyc2V5
MRQwEgYDVQQHEwtKZXJzZXkgQ2l0eTEeMBwGA1UEChMVVGhlIFVTRVJUUlVTVCBO
ZXR3b3JrMS4wLAYDVQQDEyVVU0VSVHJ1c3QgUlNBIENlcnRpZmljYXRpb24gQXV0
aG9yaXR5MIICIjANBgkqhkiG9w0BAQEFAAOCAg8AMIICCgKCAgEAgBJlFzYOw9sI
s9CsVw127c0n00ytUINh4qogTQktZAnczomfzD2p7PbPwdzx07HWezcoEStH2jnG
vDoZtF+mvX2do2NCtnbyqTsrkfjib9DsFiCQCT7i6HTJGLSR1GJk23+jBvGIGGqQ
Ijy8/hPwhxR79uQfjtTkUcYRZ0YIUcuGFFQ/vDP+fmyc/xadGL1RjjWmp2bIcmfb
IWax1Jt4A8BQOujM8Ny8nkz+rwWWNR9XWrf/zvk9tyy29lTdyOcSOk2uTIq3XJq0
tyA9yn8iNK5+O2hmAUTnAU5GU5szYPeUvlM3kHND8zLDU+/bqv50TmnHa4xgk97E
xwzf4TKuzJM7UXiVZ4vuPVb+DNBpDxsP8yUmazNt925H+nND5X4OpWaxKXwyhGNV
icQNwZNUMBkTrNN9N6frXTpsNVzbQdcS2qlJC9/YgIoJk2KOtWbPJYjNhLixP6Q5
D9kCnusSTJV882sFqV4Wg8y4Z+LoE53MW4LTTLPtW//e5XOsIzstAL81VXQJSdhJ
WBp/kjbmUZIO8yZ9HE0XvMnsQybQv0FfQKlERPSZ51eHnlAfV1SoPv10Yy+xUGUJ
5lhCLkMaTLTwJUdZ+gQek9QmRkpQgbLevni3/GcV4clXhB4PY9bpYrrWX1Uu6lzG
KAgEJTm4Diup8kyXHAc/DVL17e8vgg8CAwEAAaOB8jCB7zAfBgNVHSMEGDAWgBSg
EQojPpbxB+zirynvgqV/0DCktDAdBgNVHQ4EFgQUU3m/WqorSs9UgOHYm8Cd8rID
ZsswDgYDVR0PAQH/BAQDAgGGMA8GA1UdEwEB/wQFMAMBAf8wEQYDVR0gBAowCDAG
BgRVHSAAMEMGA1UdHwQ8MDowOKA2oDSGMmh0dHA6Ly9jcmwuY29tb2RvY2EuY29t
L0FBQUNlcnRpZmljYXRlU2VydmljZXMuY3JsMDQGCCsGAQUFBwEBBCgwJjAkBggr
BgEFBQcwAYYYaHR0cDovL29jc3AuY29tb2RvY2EuY29tMA0GCSqGSIb3DQEBDAUA
A4IBAQAYh1HcdCE9nIrgJ7cz0C7M7PDmy14R3iJvm3WOnnL+5Nb+qh+cli3vA0p+
rvSNb3I8QzvAP+u431yqqcau8vzY7qN7Q/aGNnwU4M309z/+3ri0ivCRlv79Q2R+
/czSAaF9ffgZGclCKxO/WIu6pKJmBHaIkU4MiRTOok3JMrO66BQavHHxW/BBC5gA
CiIDEOUMsfnNkjcZ7Tvx5Dq2+UUTJnWvu6rvP3t3O9LEApE9GQDTF1w52z97GA1F
zZOFli9d31kWTz9RvdVFGD/tSo7oBmF0Ixa1DVBzJ0RHfxBdiSprhTEUxOipakyA
vGp4z7h/jnZymQyd/teRCBaho1+V
-----END CERTIFICATE-----
)"));
		certstore->add(
			std::move(certchain),
			RsaPrivateKey::fromPem(R"(-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQCqMVOpViuYgF2X
y6yjmOZXMu8/pjC9nGGZFtF1yXguo2A8mEhgDyaIHiIHdybCVo2bx8Z4+/CiRozu
Y/AfJM+AlXFemhMeBMSb7RVEs5IFx4Zb0qNqdE07VsQvom971r/uZAUs2+JnSih4
ms/bIXCzSge+99OcUgZyowDxhJYGmUcI8DKlEmz8o8eCq3ocj0GQzlNGkUylG0DQ
2ymG3hJplJLtBU6WqN+RjbCa8MyihSIM1gBimPkIssCDHjhn9HU6l+tBkDHl5Cm7
nom7hSgNdn6FfERqI3U/mzyHUUMshMp7VJG7vlDJJyfW7D2ykxMkIL+Z2OtjvgNp
WBvPVG+zAgMBAAECggEAAzoWM2Xxdt3DaIcxfPr/YXRGYJ2R22myPzw7uN3ODCXu
EDGoknGwsfBoUsRQLtHqgD0K2h/+XjiAn/bmUzpxpY18oP+PRAikT0e9suTFhjVU
EQk7lSwi8fB7BDAydVWk1ywV6qJsqeqx1vLDsb++xEqvpOl/NwqMs4widQtytymu
4n7/5OJik0wMNwSoBApOdRgX4EeGmbPjZj+U8zu1h+xVGDLSAd9stYsZ7jktAZVc
NIiBmNk+d0Laywq+XdD+t3PrbT/IbvqOlq/tAvMI7mAs3t/g6xYWABR6YzkMa0FV
xywzICEgum/ssilWWgnxlAdmhONC/5UNRtg1QflsaQKBgQDkOVN3uTEFuLXnsvyp
IKSxRXnIOc+1RHJiVAZhMGD3Kjr8tuAfTwHFng6CFV6vwAAhli1zU8UJw7U/9rph
aIzNk02RMAPMWQYk1nfUlQkzniG0ydhzI48yEvULSC6t+KKBaQYvmNu6a6pSh+aj
R08r9EzVNRXI9pV22mC+g5C7zQKBgQC+5/JFg55FFyLBzR0SMKHRj6gR1WC0Vovh
tu69yVpg/8JdXUPr7vmtgk617vLP9yttQ4rmBsjeUCG1jtWFDSI9dgtVqolfK+qX
0bh3fmdgolxmta0B51CWdF57zhBnPSoOSuI+d+C4p3AS5Ay1SfPsOCfGu+mZ6KLf
Ee+jYzFZfwKBgQCM7nGCnxOMqvF5sOehMQ1CgtqfMEP5ddkEq0p9PbjDKIrgf7WK
3+kCNYZUAgpEkVYDZ4+Nhg9I5lfItf2GJV+9mtbtby8JQ3gty1qYJahW/bFmyLYm
87B7hYVYgCyDNeRz8Xzma4hUaCP3bwCXl3NmeyfvCSb4wHyvtk7Dls8LiQKBgFZr
IxXqreOyxG4cjtNkJmx57mgcQomAQBQuPka1dm9Ad9jR1mRgKrArs7vR7iLMTeFJ
WQAmBBn3Bjts7CUtu9k8rYbbCxKFC84sBqg5FUz+UnvANBAPiUCCbx72OiCx5G7R
4TbMB3MvgKFckJAkaQH+rard97JPSCNYuDUrOvS7AoGAPRqzqsY1NuSX4NET/5kX
WNpI0C1Y02SodiZEOJiSd1lZdOs+RzKJv0yGZ4bTGzF5g0pPQzRVh7X/RkqvOooi
AdlKGykSXMNzrdgShNxr/RjC+n9+a4pfZWnW8eMbCJWW0ptjycNRbU/rLwmLSuV8
SOEKVYljbu9o5nFbg1zU0Ck=
-----END PRIVATE KEY-----)")
		);
	}

	Server serv;
	ServerWebService web_srv;
	web_srv.should_accept_websocket_connection = [](Socket& s, const HttpRequest& req, ServerWebService&)
	{
		if (req.path == "/r1")
		{
			s.custom_data.getStructFromMap(ClientData).supports_report_ids = true;
		}

#if DEBUG
		return true;
#else
		if (auto origin = req.findHeader("Origin"))
		{
			// Run MessageBoxA off-thread because otherwise explorer freezes if the user confirms the prompt via a keyboard press (enter or spacebar). Can't make this shit up.
			static bool res;
			Thread thrd([](Capture&& cap)
			{
				res = (MessageBoxA(0, cap.get<std::string>().c_str(), "WebHID for Firefox", MB_YESNO) == IDYES);
			}, std::string("Allow the page at " + *origin + " to access your HID devices?"));
			thrd.awaitCompletion();
			return res;
		}
		return false;
#endif
	};
	web_srv.on_websocket_message = [](WebSocketMessage& msg, Socket& s, ServerWebService&)
	{
		if (msg.is_text)
		{
			if (msg.data == "list")
			{
				for (const auto& hid : hwHid::getAll())
				{
					if (hid_is_permitted(hid))
					{
						std::string msg = "dev:";
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
						ServerWebService::wsSendText(s, std::move(msg));
					}
				}
				ServerWebService::wsSendText(s, "dev");
			}
			else if (msg.data.substr(0, 4) == "open")
			{
				const uint32_t hid_hash = std::strtoul(msg.data.c_str() + 4, nullptr, 10);
				if (s.custom_data.getStructFromMap(ClientData).findSubscription(hid_hash) == nullptr)
				{
					for (auto& hid : hwHid::getAll())
					{
						if (hid_is_permitted(hid) && hid_to_hash(hid) == hid_hash)
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
			StringRefReader r(msg.data);
			uint8_t msgid; r.u8(msgid);
			if (msgid == 0)
			{
				if (msg.data.size() >= 5)
				{
					uint32_t hid_hash; r.u32_be(hid_hash);
					for (auto& hid : hwHid::getAll())
					{
						if (hid_is_permitted(hid) && hid_to_hash(hid) == hid_hash)
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
						if (hid_is_permitted(hid) && hid_to_hash(hid) == hid_hash)
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
		}
	};
	if (!serv.bindCrypto(33881, &web_srv, std::move(certstore)))
	{
#if DEBUG
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
