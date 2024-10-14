(function()
{
	class HIDDevice extends EventTarget
	{
		_satisfiesOptions(options)
		{
			for (const filter of options.filters)
			{
				if (this._satisfiesFilter(filter))
				{
					return true;
				}
			}
			return false;
		}

		_satisfiesFilter(filter)
		{
			if ("vendorId" in filter && filter.vendorId != this.vendorId)
			{
				return false;
			}
			if ("productId" in filter && filter.productId != this.productId)
			{
				return false;
			}
			if ("usagePage" in filter || "usage" in filter)
			{
				for (const collection of this.collections)
				{
					if (collection._satisfiesFilter(filter))
					{
						return true;
					}
				}
				return false;
			}
			return true;
		}
	}
	window.HIDDevice = HIDDevice;

	class HIDCollectionInfo
	{
		_satisfiesFilter(filter)
		{
			if ("usagePage" in filter && filter.usagePage != this.usagePage)
			{
				return false;
			}
			if ("usage" in filter && filter.usage != this.usage)
			{
				return false;
			}
			return true;
		}
	}

	class HIDInputReportEvent extends Event
	{
		constructor(data)
		{
			super("inputreport");
			this.data = data;
		}
	}
	window.HIDInputReportEvent = HIDInputReportEvent;

	class HIDConnectionEvent extends Event
	{
		constructor(type, device)
		{
			super(type);
			this.device = device;
		}
	}
	window.HIDConnectionEvent = HIDConnectionEvent;

	if (!localStorage.getItem("WebHID for Firefox: requested devices"))
	{
		localStorage.setItem("WebHID for Firefox: requested devices", "[]");
	}
	const requested_devices = JSON.parse(localStorage.getItem("WebHID for Firefox: requested devices"));
	const save_requested_devices = () => localStorage.setItem("WebHID for Firefox: requested devices", JSON.stringify(requested_devices));

	let devlist = [];
	const hash_to_dev = {};
	let active_subscriptions = 0;
	let ws_promise, devlist_resolve;
	const get_ws = function()
	{
		if (!ws_promise)
		{
			ws_promise = new Promise(function(resolve, reject)
			{
				const ws = new WebSocket("wss://127-0-0-1.p2ptls.com:33881/");
				ws.binaryType = "arraybuffer";
				ws.onopen = function()
				{
					resolve(ws);
				};
				ws.onerror = function(event)
				{
					reject("Failed to connect to local WebHID provider server");
					ws_promise = undefined;
				};
				ws.onmessage = function(event)
				{
					if (event.data instanceof ArrayBuffer)
					{
						const view = new DataView(event.data);
						switch (view.getUint8(0))
						{
						case 0:
							const hash = view.getUint32(1);
							const dev = hash_to_dev[hash];
							const evt = new HIDInputReportEvent(new DataView(event.data.slice(5)));
							dev.dispatchEvent(evt);
							if ("oninputreport" in dev && typeof dev.oninputreport == "function")
							{
								dev.oninputreport(evt);
							}
							break;
						}
					}
					else
					{
						const msg = event.data.split(":");
						switch (msg[0])
						{
						case "dev":
							if (msg.length > 1)
							{
								const hash = parseInt(msg[1]);
								if (hash in hash_to_dev)
								{
									devlist.push(hash_to_dev[hash]);
								}
								else
								{
									const dev = new HIDDevice();
									dev._hash = hash;
									dev._physicalHash = parseInt(msg[2]);
									dev.vendorId = parseInt(msg[3]);
									dev.productId = parseInt(msg[4]);
									dev.productName = msg[5];
									dev.collections = [];
									dev.opened = false;
									dev.open = async function()
									{
										console.assert(!this.opened);

										ws.send("open" + this._hash);
										this.opened = true;
										++active_subscriptions;
									};
									dev.close = async function()
									{
										console.assert(this.opened);

										ws.send("clse" + this._hash);
										this.opened = false;
										--active_subscriptions;
									};
									dev.forget = async function()
									{
										await this.close();

										const index = requested_devices.indexOf(this._physicalHash);
										if (index !== -1)
										{
											requested_devices.splice(index, 1);
											save_requested_devices();
										}
									};
									dev.sendReport = async function(reportId, data)
									{
										console.assert(this.opened);

										// prepend: msg id (1 byte) + hid hash (4 bytes) + report id (1 byte)
										const msg = new Uint8Array(data.byteLength + 6);
										msg.set([0], 0); // msg id
										msg.set([this._hash >> 24, (this._hash >> 16) & 0xff, (this._hash >> 8) & 0xff, this._hash & 0xff], 1); // hid hash
										msg.set([reportId], 5); // report id
										msg.set(data, 6); // data
										ws.send(msg);
									};
									dev.sendFeatureReport = async function(reportId, data)
									{
										console.assert(this.opened);

										// prepend: msg id (1 byte) + hid hash (4 bytes) + report id (1 byte)
										const msg = new Uint8Array(data.byteLength + 6);
										msg.set([1], 0); // msg id
										msg.set([this._hash >> 24, (this._hash >> 16) & 0xff, (this._hash >> 8) & 0xff, this._hash & 0xff], 1); // hid hash
										msg.set([reportId], 5); // report id
										msg.set(data, 6); // data
										ws.send(msg);
									};
									dev.receiveFeatureReport = () => { console.log("TODO: HIDDevice.receiveFeatureReport"); };

									const collection = new HIDCollectionInfo();
									collection.usage = parseInt(msg[6]);
									collection.usagePage = parseInt(msg[7]);
									dev.collections.push(collection);

									// Push HIDReportInfo shims, subtracting report id from length
									// TODO: The reportId field here would be needed for Razer devices
									collection.inputReports = [ { items: [ { reportSize: 8, reportCount: parseInt(msg[8]) - 1 } ] } ];
									collection.outputReports = [ { items: [ { reportSize: 8, reportCount: parseInt(msg[9]) - 1 } ] } ];
									collection.featureReports = [ { items: [ { reportSize: 8, reportCount: parseInt(msg[10]) - 1 } ] } ];

									devlist.push(dev);
									hash_to_dev[hash] = dev;
								}
							}
							else
							{
								devlist_resolve();
							}
							break;

						case "stopped":
							const dev = hash_to_dev[parseInt(msg[1])];
							dev.opened = false;
							--active_subscriptions;
							navigator.hid.dispatchEvent(new HIDConnectionEvent("disconnect", dev));
							// TODO: We might want to periodically check for requested devices to fire "connect" event.
							break;
						}
					}
				};
			});
		}
		return ws_promise;
	};
	const update_devlist = async function()
	{
		const ws = await get_ws();
		devlist = [];
		const devlist_promise = new Promise(resolve => devlist_resolve = resolve);
		ws.send("list");
		return devlist_promise;
	};

	class HID extends EventTarget {};
	window.HID = HID;

	navigator.hid = new HID();
	navigator.hid.getDevices = async function()
	{
		const matching_devices = [];
		if (active_subscriptions == 0) // Not updating the dev list when we're already actively talking with a HID because our bandwidth is somewhat limited and timing is sensitive for some use cases.
		{
			await update_devlist();
		}
		for (const dev of devlist)
		{
			if (requested_devices.includes(dev._physicalHash))
			{
				matching_devices.push(dev);
			}
		}
		//console.log("getDevices matches:", matching_devices);
		return matching_devices;
	};
	navigator.hid.requestDevice = async function(options)
	{
		console.assert("filters" in options);

		const matching_devices = [];
		await update_devlist();
		for (const dev of devlist)
		{
			if (dev._satisfiesOptions(options))
			{
				matching_devices.push(dev);
			}
		}

		if (matching_devices.length > 1)
		{
			console.log("TODO: Prompt user to narrow down device list" /* if different physical devices */, matching_devices);
			return [];
		}

		for (const dev of matching_devices)
		{
			if (!requested_devices.includes(dev._physicalHash))
			{
				requested_devices.push(dev._physicalHash);
				save_requested_devices();
			}
		}
		return matching_devices;
	};
})();
