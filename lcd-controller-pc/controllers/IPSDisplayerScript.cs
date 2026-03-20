using Godot;
using System;
using System.Collections.Generic;

public partial class IPSDisplayerScript : TextureRect
{
	// Local buffer to store serial messages dequeued from ComButton
	private readonly List<string> _serialBuffer = new();
	//private OptionButton ComPort = GetNode<ComButton>("../controllers/ComButton");

	// Called when the node enters the scene tree for the first time.
	public override void _Ready()
	{

	}

	// Called every frame. 'delta' is the elapsed time since the previous frame.
	public override void _Process(double delta)
	{
	}
}
