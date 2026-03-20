using Godot;
using System;
using System.IO.Ports;

public partial class ComSendController : LineEdit
{
	// Called when the node enters the scene tree for the first time.
	public override void _Ready()
	{

	}

	private void _on_text_submitted(string text)
	{
		// Use provided text (from signal) or the LineEdit's Text as fallback
		var toSend = string.IsNullOrEmpty(text) ? Text : text;
		if (string.IsNullOrEmpty(toSend))
		{
			return;
		}

		try
		{
			var sp = ComButton.ComPortInfo._serialPort;
			if (sp != null && sp.IsOpen)
			{
				sp.WriteLine(toSend);
				GD.Print("Sent to COM: " + toSend);
			}
			else
			{
				GD.PrintErr("Serial port not open. Cannot send.");
			}
		}
		catch (Exception ex)
		{
			GD.PrintErr("Send error: " + ex.Message);
		}

		// clear input after sending
		Text = string.Empty;
	}

	// Called every frame. 'delta' is the elapsed time since the previous frame.
	public override void _Process(double delta)
	{
	}
}
