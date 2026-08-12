(function()
{
	class HIDDevice extends EventTarget
	{
		_satisfiesOptions(options)
		{
			if (options.filters.length == 0)
			{
				return true;
			}
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
			if (filter.vendorId !== undefined && filter.vendorId != this.vendorId)
			{
				return false;
			}
			if (filter.productId !== undefined && filter.productId != this.productId)
			{
				return false;
			}
			if (filter.usagePage !== undefined || filter.usage !== undefined)
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

		_getDisplayName()
		{
			return this.productName
				? this.productName
				: "Unknown Device (" + this.vendorId.toString(16).toUpperCase().padStart(4, "0") + ":" + this.productId.toString(16).toUpperCase().padStart(4, "0") + ")"
				;
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

	class HIDReportInfo
	{
	}

	class HIDReportItem
	{
	}

	class HIDInputReportEvent extends Event
	{
		constructor(device, data)
		{
			super("inputreport");
			this.data = data;
			this.device = device;
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

	const requested_devices = JSON.parse(localStorage.getItem("WebHID for Firefox: requested devices") ?? "[]");
	const save_requested_devices = () => localStorage.setItem("WebHID for Firefox: requested devices", JSON.stringify(requested_devices));

	let devlist = [];
	const hash_to_dev = {};
	const phys_hash_to_dev = {};
	let active_subscriptions = 0;
	let ws_promise, devlist_promise, devlist_resolve;
	const get_ws = function()
	{
		if (!ws_promise)
		{
			ws_promise = new Promise(function(resolve, reject)
			{
				const ws = new WebSocket("wss://127-0-0-1.faketls.com:33881/r1");
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
						case 0: // input report from server version 0.1.0
							{
								const hash = view.getUint32(1);
								const dev = hash_to_dev[hash];
								const evt = new HIDInputReportEvent(dev, new DataView(event.data.slice(5)));
								dev.dispatchEvent(evt);
								if ("oninputreport" in dev && typeof dev.oninputreport == "function")
								{
									dev.oninputreport(evt);
								}
							}
							break;

						case 1: // input report with reportId
							{
								const hash = view.getUint32(1);
								const dev = hash_to_dev[hash];
								const evt = new HIDInputReportEvent(dev, new DataView(event.data.slice(6)));
								evt.reportId = view.getUint8(5);
								dev.dispatchEvent(evt);
								if (typeof dev.oninputreport == "function")
								{
									dev.oninputreport(evt);
								}
							}
							break;

						case 2: // feature report
							{
								const hash = view.getUint32(1);
								const dev = hash_to_dev[hash];
								dev._featureReportResolve(new DataView(event.data.slice(6)));
							}
							break;
						}
					}
					else
					{
						const msg = event.data.split(":");
						switch (msg[0])
						{
						case "ver": // sent on establish as of 0.1.4
							console.log("WebHID for Firefox: Local server is on version "+msg[1]);
							break;

						case "dev":
							if (msg.length > 1)
							{
								const hidHash = parseInt(msg[1]);
								const physHash = parseInt(msg[2]);
								const vendorId = parseInt(msg[3]);
								const collapseIntoSameDevice = (vendorId == 0x8089); // Shitty fix for Sayodevice which expects its collections to be grouped in the same HIDDevice even tho that's not how they present it to the OS.
								const hash = (collapseIntoSameDevice ? physHash : hidHash);

								let dev;
								if (collapseIntoSameDevice)
								{
									dev = phys_hash_to_dev[physHash];
									if (dev && devlist.indexOf(dev) == -1)
									{
										devlist.push(dev);
									}
								}
								else
								{
									dev = hash_to_dev[hash];
									if (dev)
									{
										devlist.push(dev);
									}

								}

								if (!dev)
								{
									dev = new HIDDevice();
									dev._physicalHash = physHash;
									dev.vendorId = vendorId;
									dev.productId = parseInt(msg[4]);
									dev.productName = msg[5];
									dev.collections = [];
									dev.opened = false;
									dev.open = async function()
									{
										//console.info("open device", this);
										console.assert(!this.opened);

										for (const coll of this.collections)
										{
											ws.send("open" + coll._hash);
											++active_subscriptions;
										}
										this.opened = true;
									};
									dev.close = async function()
									{
										//console.info("close device", this);
										console.assert(this.opened);

										for (const coll of this.collections)
										{
											ws.send("clse" + coll._hash);
											--active_subscriptions;
										}
										this.opened = false;
									};
									dev.forget = async function()
									{
										//console.info("forget device", this);
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

										let some = false;
										for (const coll of this.collections)
										{
											if (coll.outputReports.some(x => x.reportId == reportId))
											{
												// prepend: msg id (1 byte) + hid hash (4 bytes) + report id (1 byte)
												const msg = new Uint8Array(data.byteLength + 6);
												msg.set([0], 0); // msg id
												msg.set([coll._hash >> 24, (coll._hash >> 16) & 0xff, (coll._hash >> 8) & 0xff, coll._hash & 0xff], 1); // hid hash
												msg.set([reportId], 5); // report id
												msg.set(data, 6); // data
												ws.send(msg);
												//console.info(`sendReport: sending reportId ${reportId} to collection with usage=${coll.usage}, usagePage=${coll.usagePage}`);
												some = true;
											}
										}
										if (!some)
										{
											console.warn(`sendReport: failed to find collection for reportId ${reportId}`);
										}
									};
									dev.sendFeatureReport = async function(reportId, data)
									{
										console.assert(this.opened);

										let some = false;
										for (const coll of this.collections)
										{
											if (coll.featureReports.some(x => x.reportId == reportId))
											{
												// prepend: msg id (1 byte) + hid hash (4 bytes) + report id (1 byte)
												const msg = new Uint8Array(data.byteLength + 6);
												msg.set([1], 0); // msg id
												msg.set([coll._hash >> 24, (coll._hash >> 16) & 0xff, (coll._hash >> 8) & 0xff, coll._hash & 0xff], 1); // hid hash
												msg.set([reportId], 5); // report id
												msg.set(data, 6); // data
												ws.send(msg);
												//console.info(`sendFeatureReport: sending reportId ${reportId} to collection with usage=${coll.usage}, usagePage=${coll.usagePage}`);
												some = true;
											}
										}
										if (!some)
										{
											console.warn(`sendFeatureReport: failed to find collection for reportId ${reportId}`);
										}
									};
									dev.receiveFeatureReport = function(reportId)
									{
										console.assert(this.opened);

										for (const coll of this.collections)
										{
											if (coll.featureReports.some(x => x.reportId == reportId))
											{
												const msg = new Uint8Array(6);
												msg.set([2], 0); // msg id
												msg.set([coll._hash >> 24, (coll._hash >> 16) & 0xff, (coll._hash >> 8) & 0xff, coll._hash & 0xff], 1); // hid hash
												msg.set([reportId], 5); // report id
												ws.send(msg);
												//console.info(`receiveFeatureReport: routing reportId ${reportId} to collection with usage=${coll.usage}, usagePage=${coll.usagePage}`);
												return new Promise(resolve => this._featureReportResolve = resolve);
											}
										}
										console.warn(`receiveFeatureReport: failed to find collection for reportId ${reportId}`);
									};

									devlist.push(dev);
									phys_hash_to_dev[physHash] = dev;
								}
								hash_to_dev[hidHash] = dev;

								if (!dev.collections.some(c => c._hash == hidHash))
								{
									const usage = parseInt(msg[6]);
									const usagePage = parseInt(msg[7]);

									const collection = new HIDCollectionInfo();
									collection._hash = hidHash;
									collection.usage = usage;
									collection.usagePage = usagePage;
									dev.collections.push(collection);

									const reportIds = msg[11] ? msg[11].split(",").map(x => parseInt(x)) : [0];
									collection.inputReports = [];
									collection.outputReports = [];
									collection.featureReports = [];
									const create_hid_report_info = function (arr, reportId, length)
									{
										if (length)
										{
											const item = new HIDReportItem();
											item.reportSize = 8;
											item.reportCount = length - 1; // excluding report id
											item.usages = [(usagePage << 16) | usage];

											const info = new HIDReportInfo();
											info.reportId = reportId;
											info.items = [item];
											arr.push(info);
										}
									};
									for (const reportId of reportIds)
									{
										create_hid_report_info(collection.inputReports, reportId, parseInt(msg[8]));
										create_hid_report_info(collection.outputReports, reportId, parseInt(msg[9]));
										create_hid_report_info(collection.featureReports, reportId, parseInt(msg[10]));
									}
								}
							}
							else
							{
								devlist_resolve();
							}
							break;

						case "stopped":
							const dev = hash_to_dev[parseInt(msg[1])];
							if (dev.opened)
							{
								dev.opened = false;
								--active_subscriptions;
								navigator.hid.dispatchEvent(new HIDConnectionEvent("disconnect", dev));
							}
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
		if (!devlist_promise)
		{
			const ws = await get_ws();
			devlist = [];
			devlist_promise = new Promise(resolve => devlist_resolve = resolve);
			ws.send("list");
		}
		await devlist_promise;
		devlist_promise = undefined;
	};
	const create_connect_prompt = function()
	{
		return new Promise(function(resolve, reject)
		{
			let div = document.createElement("div");
			div.id = "webhid-for-firefox-popup";
			div.textContent = "This website wants to connect to a HID device";
			div.style.position = "fixed";
			div.style.top = "0";
			div.style.left = "0";
			div.style.background = "#fff";
			div.style.color = "#000";
			div.style.padding = "15px";
			div.style.fontFamily = "sans-serif";
			div.style.boxShadow = "5px 5px 10px 0px #000";
			div.style.zIndex = "99999999999";
			{
				const fieldset = document.createElement("fieldset");
				fieldset.style.border = "2px groove ThreeDFace"; // maintain UA style
				fieldset.style.margin = "10px 0";
				div.appendChild(fieldset);
			}
			{
				const button = document.createElement("button");
				button.style.float = "right";
				button.style.border = "2px outset ButtonBorder"; // maintain UA style
				button.style.backgroundColor = "ButtonFace"; // maintain UA style
				button.style.marginLeft = "4px";
				button.textContent = "Cancel";
				button.onclick = function()
				{
					document.documentElement.removeChild(div);
					resolve();
				};
				div.appendChild(button);
			}
			{
				const button = document.createElement("button");
				button.style.float = "right";
				button.style.border = "2px outset ButtonBorder"; // maintain UA style
				button.style.backgroundColor = "ButtonFace"; // maintain UA style
				button.textContent = "Connect";
				button.onclick = function()
				{
					const input = document.querySelector("[name=webhid-for-firefox-device]:checked");
					if (input)
					{
						resolve(parseInt(input.id.substr(26)));
						document.documentElement.removeChild(div);
					}
				};
				div.appendChild(button);
			}
			div = document.documentElement.appendChild(div);
		});
	};
	const update_connect_prompt = function(devices)
	{
		const fieldset = document.querySelector("#webhid-for-firefox-popup fieldset");
		if (!fieldset)
		{
			return; // Race condition
		}

		if (fieldset.textContent == "No compatible devices found")
		{
			fieldset.innerHTML = "";
		}
		else
		{
			// Handle removed devices
			const arr = document.querySelectorAll("[name=webhid-for-firefox-device]");
			for (let i = 0; i != arr.length; ++i)
			{
				if (!devices[arr[i].id.substr(26)])
				{
					arr[i].parentNode.parentNode.removeChild(arr[i].parentNode);
				}
			}
		}

		// Handle added devices
		for (const [i, device] of Object.entries(devices))
		{
			if (!document.getElementById("webhid-for-firefox-device-" + i))
			{
				const group = document.createElement("div");
				group.style.padding = "6px 0";
				{
					const input = document.createElement("input");
					input.id = "webhid-for-firefox-device-" + i;
					input.name = "webhid-for-firefox-device";
					input.type = "radio";
					group.appendChild(input);
				}
				{
					const label = document.createElement("label");
					label.setAttribute("for", "webhid-for-firefox-device-" + i);
					label.textContent = device[0]._getDisplayName();
					group.appendChild(label);
				}
				fieldset.appendChild(group);
			}
		}

		const first = document.querySelector("[name=webhid-for-firefox-device]");
		if (first)
		{
			// If no device is checked, check the first one.
			if (!document.querySelector("[name=webhid-for-firefox-device]:checked"))
			{
				first.checked = true;
			}
		}
		else
		{
			// No devices found; say this explicitly instead of showing an empty fieldset.
			fieldset.textContent = "No compatible devices found";
		}
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
		//console.info("getDevices returns:", matching_devices);
		return matching_devices;
	};
	navigator.hid.requestDevice = async function(options)
	{
		if (!("filters" in options))
		{
			throw new Error("'filters' must be given");
		}
		if (document.getElementById("webhid-for-firefox-popup"))
		{
			throw new Error("A device request is already in progress");
		}

		const get_matching_physical_devices = async function()
		{
			const matching_devices = [];
			await update_devlist();
			for (const dev of devlist)
			{
				if (dev._satisfiesOptions(options))
				{
					matching_devices.push(dev);
				}
			}

			const matching_physical_devices = {};
			for (const dev of matching_devices)
			{
				matching_physical_devices[dev._physicalHash] ??= [];
				matching_physical_devices[dev._physicalHash].push(dev);
			}
			return matching_physical_devices;
		};

		let matching_physical_devices = await get_matching_physical_devices();
		const promise = create_connect_prompt();
		update_connect_prompt(matching_physical_devices);

		const interval = setInterval(async function()
		{
			matching_physical_devices = await get_matching_physical_devices();
			update_connect_prompt(matching_physical_devices);
		}, 1000);

		const hash = await promise;

		if (hash
			&& !requested_devices.includes(hash)
			)
		{
			requested_devices.push(hash);
			save_requested_devices();
		}

		clearInterval(interval);

		//console.info("requestDevice returns:", hash ? matching_physical_devices[hash] : []);
		return hash ? matching_physical_devices[hash] : [];
	};
})();
