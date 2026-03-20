using Godot;
using System;
using System.IO.Ports;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Management;
using System.Text.RegularExpressions;

public partial class ComButton : OptionButton
{
	// Called when the node enters the scene tree for the first time.
	public static class ComPortInfo
	{
		public static SerialPort _serialPort;
		public static string[] ports = Array.Empty<string>();
		public static int baudRate = 9600;
	}
	private readonly ConcurrentQueue<string> _rxQueue = new();
	private readonly ConcurrentQueue<string> _bufferQueue = new();
	private readonly Dictionary<string, ManagementBaseObject> _deviceMap = new();

	private float _popupTimer = 0f;
	private PopupPanel _popupNode; // 声明但不立即 new
	private Label _popupLabel;

	public override void _Ready()
	{
		// 1. 初始化弹窗节点（如果场景里没有，就代码创建一个）
		_popupNode = GetNodeOrNull<PopupPanel>("PopupPanel");
		
		if (_popupNode == null)
		{
			_popupNode = new PopupPanel();
			_popupLabel = new Label();
			_popupNode.AddChild(_popupLabel);
			AddChild(_popupNode); // 必须加入场景树
		}
		else
		{
			_popupLabel = _popupNode.GetNodeOrNull<Label>("Label") ?? new Label();
			if (_popupLabel.GetParent() == null) _popupNode.AddChild(_popupLabel);
		}

		PopulateComPorts();
	}

	private void PopulateComPorts()
	{
		while (GetItemCount() > 0)
			RemoveItem(0);

		try
		{
			ComPortInfo.ports = SerialPort.GetPortNames();
		}
		catch (Exception ex)
		{
			GD.Print("Error: " + ex.Message);
			ComPortInfo.ports = Array.Empty<string>();
		}

		// Try to populate WMI map for better classification (Windows only)
		_deviceMap.Clear();
		try
		{
			using var searcher = new ManagementObjectSearcher(
				"SELECT * FROM Win32_PnPEntity WHERE Name LIKE '%(COM%'");
			foreach (ManagementObject mo in searcher.Get())
			{
				var name = (mo["Name"] ?? string.Empty).ToString();
				var m = Regex.Match(name, @"\((COM\d+)\)");
				if (m.Success)
				{
					var portName = m.Groups[1].Value;
					_deviceMap[portName] = mo;
				}
			}
		}
		catch (Exception ex)
		{
			// WMI may not be available on non-Windows platforms or missing reference
			GD.PrintErr("WMI lookup failed: " + ex.Message);
		}

		if (ComPortInfo.ports.Length == 0)
		{
			AddItem("NO PORTS");
			Disabled = true;
		}
		else
		{
			Disabled = false;
			foreach (string port in ComPortInfo.ports)
			{
				var type = ClassifyPort(port, out var desc);
				AddItem($"{port} [{type}]");
			}
			Select(-1);
		}
	}

	private void _on_connect_button_pressed()
	{
		OpenCom();
	}

	private void _on_refresh_button_pressed()
	{
		PopulateComPorts();
		ShowDetails(400, 0, 100, 50, "Refreshed", 1.0f);
	}

	private void _on_disconnect_button_pressed()
	{
		CloseCom();
		ShowDetails(280, 80, 100, 50, "Disconnected", 1.0f);
	}

	private void OpenCom()
	{
		try
		{
			CloseCom();

			int sel = GetSelected();
			if (sel < 0)
			{
				ShowDetails(280, 80, 100, 50, "No Port Selected", 1.0f);
				return;
			}
		 // item text may include classification suffix, extract COM name
			string itemText = GetItemText(sel);
			var m = System.Text.RegularExpressions.Regex.Match(itemText, "(COM\\d+)");
			string portName = m.Success ? m.Groups[1].Value : itemText;

			ComPortInfo._serialPort = new SerialPort(portName, ComPortInfo.baudRate, Parity.None, 8, StopBits.One)
			{
				ReadTimeout = 500,
				WriteTimeout = 500,
			};

			ComPortInfo._serialPort.DataReceived += SerialPort_DataReceived;
			ComPortInfo._serialPort.Open();

			ShowDetails(280, 80, 100, 50, "Connected", 1.0f);
		}
		catch (Exception ex)
		{
			GD.PrintErr("OpenCom error: " + ex.Message);
			ShowDetails(280, 80, 100, 50, "Error", 1.0f);
			CloseCom();
		}
	}

	private void CloseCom()
	{
	  if (ComPortInfo._serialPort != null)
		{
			try
			{
				ComPortInfo._serialPort.DataReceived -= SerialPort_DataReceived;
				if (ComPortInfo._serialPort.IsOpen)
					ComPortInfo._serialPort.Close();
				ComPortInfo._serialPort.Dispose();
			}
			catch (Exception ex)
			{
				GD.PrintErr("CloseCom error: " + ex.Message);
			}
			finally
			{
				ComPortInfo._serialPort = null;
			}
		}
	}

	private void _on_bau_button_item_selected(int index)
	{
		if (index == 0)
			ComPortInfo.baudRate = 9600;
		else if (index == 1)
			ComPortInfo.baudRate = 14400;
		else if (index == 2)
			ComPortInfo.baudRate = 19200;
		else if (index == 3)
			ComPortInfo.baudRate = 28800;
		else if (index == 4)
			ComPortInfo.baudRate = 38400;
		else if (index == 5)
			ComPortInfo.baudRate = 57600;
		else if (index == 6)
			ComPortInfo.baudRate = 115200;
		else if (index == 7)
			ComPortInfo.baudRate = 230400;
		else if (index == 8)
			ComPortInfo.baudRate = 250000;
		else if (index == 9)
			ComPortInfo.baudRate = 460800;
		else if (index == 10)
			ComPortInfo.baudRate = 500000;
		else if (index == 11)
			ComPortInfo.baudRate = 921600;
		else if (index == 12)
			ComPortInfo.baudRate = 1000000;
		else if (index == 13)
			ComPortInfo.baudRate = 3000000;
		else if (index == 14)
			ComPortInfo.baudRate = 7500000;
		else if (index == 15)
			ComPortInfo.baudRate = 10000000;
	}

	// classify port using WMI data when available
	private string ClassifyPort(string port, out string description)
	{
		description = string.Empty;
		if (_deviceMap.TryGetValue(port, out var mo))
		{
			var pnp = (mo["PNPDeviceID"] ?? string.Empty).ToString();
			var caption = (mo["Caption"] ?? string.Empty).ToString();
			var manufacturer = (mo["Manufacturer"] ?? string.Empty).ToString();
			description = $"{caption} ({manufacturer})";

			if (pnp.IndexOf("BTHENUM", StringComparison.OrdinalIgnoreCase) >= 0
				|| caption.IndexOf("Bluetooth", StringComparison.OrdinalIgnoreCase) >= 0)
				return "Bluetooth";

			if (pnp.IndexOf("USB", StringComparison.OrdinalIgnoreCase) >= 0
				|| caption.IndexOf("USB", StringComparison.OrdinalIgnoreCase) >= 0
				|| pnp.IndexOf("FTDIBUS", StringComparison.OrdinalIgnoreCase) >= 0
				|| caption.IndexOf("CP210", StringComparison.OrdinalIgnoreCase) >= 0
				|| caption.IndexOf("CH340", StringComparison.OrdinalIgnoreCase) >= 0)
				return "USB Serial";

			return "PNP Device";
		}

		return "Unknown";
	}


	private void SerialPort_DataReceived(object sender, SerialDataReceivedEventArgs e)
	{
		try
		{
			var sp = (SerialPort)sender;
			// 非阻塞读取所有可用数据并入队到主线程处理
			string s = sp.ReadExisting();
			if (!string.IsNullOrEmpty(s))
			{
				// 拆成行，逐行入队到内部处理队列和外部可访问缓冲区
				var parts = s.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries);
				foreach (var part in parts)
				{
					var line = part.Trim();
					if (line.Length == 0) continue;
					_rxQueue.Enqueue(line);
					_bufferQueue.Enqueue(line);
				}
			}
		}
		catch (Exception ex)
		{
			GD.PrintErr("DataReceived error: " + ex.Message);
		}
	}

	public void ShowDetails(int x, int y,int w,int h, string info, float time)
	{
		if (_popupNode == null || _popupLabel == null) return;

		// 设置文本
		_popupLabel.Text = info;
		_popupLabel.HorizontalAlignment = HorizontalAlignment.Center;
		_popupLabel.VerticalAlignment = VerticalAlignment.Center;
		_popupLabel.SetAnchorsAndOffsetsPreset(Control.LayoutPreset.FullRect);
		// 设置位置
		// Position 是相对于父节点的，如果是全局弹出，建议使用 Window 坐标
		_popupNode.Position = new Vector2I(x, y);
		_popupNode.Size = new Vector2I(w, h);
		// 显示弹窗
		if (!_popupNode.Visible)
		{
			_popupNode.Popup();
		}

		_popupTimer = time;
	}

	// Called every frame. 'delta' is the elapsed time since the previous frame.
	public override void _Process(double delta)
	{
		// 1. 处理串口接收
		while (_rxQueue.TryDequeue(out var line))
		{
			GD.Print(line);
		}

		if (_popupTimer > 0)
		{
			_popupTimer -= (float)delta;
			if (_popupTimer <= 0)
			{
				_popupNode?.Hide();
			}
		}
	}
	
	// 外部可调用：获取当前缓冲区快照（不清除）
	public string[] PeekReceiveBuffer()
	{
		return _bufferQueue.ToArray();
	}

	// 外部可调用：清空并返回缓冲区中的所有项
	public string[] DequeueAllReceiveBuffer()
	{
		var list = new System.Collections.Generic.List<string>();
		while (_bufferQueue.TryDequeue(out var item))
			list.Add(item);
		return list.ToArray();
	}
}
