/*browser.webRequest.onHeadersReceived.addListener(
	details => {
		const headers = details.responseHeaders.filter(
			h => h.name.toLowerCase() !== "content-security-policy"
			);
		return { responseHeaders: headers };
	},
	{ urls: ["https://wootility.io/"] },
	["blocking", "responseHeaders"]
);*/

browser.webRequest.onBeforeRequest.addListener(
	(details) => {
		const filter = browser.webRequest.filterResponseData(details.requestId);
		const decoder = new TextDecoder("utf-8");
		const encoder = new TextEncoder();

		filter.ondata = (event) => {
			let str = decoder.decode(event.data, { stream: true });
			str = str.replace(
				/<meta\s+[^>]*http-equiv=["']?\s*Content-Security-Policy\s*["']?[^>]*>/gi,
				"<!-- CSP stripped by WebHID for Firefox -->"
			);
			filter.write(encoder.encode(str));
			filter.disconnect();
		};

		return {};
	},
	{
		urls: [
			"https://wootility.io/",
			"https://wootility.io/index.html",
			"https://beta.wootility.io/",
			"https://beta.wootility.io/index.html",
		],
		types: ["main_frame", "xmlhttprequest"]
	},
	["blocking"]
);
