using Sandbox;
using Sandbox.Movement;

public class FlyMove : MoveMode
{
	public float Acceleration { get; set; } = 10f;
	public float Friction { get; set; } = 4f;
	public float StopSpeed { get; set; } = 100f;


	public override int Score( PlayerController controller )
	{
		if (controller.GetComponent<Player>().IsPlayMode)
		{
			return -1;
		}
		return 100;
	}

	public override void UpdateRigidBody( Rigidbody body )
	{
		body.Gravity = false;
		body.LinearDamping = 0f;   // we do friction ourselves in AddVelocity
		body.AngularDamping = 1f;
	}

	public override Vector3 UpdateMove( Rotation eyes, Vector3 input )
	{
		input = input.ClampLength( 1f );
		Vector3 wishDir = eyes * input;

		bool isRunning = Input.Down( Controller.AltMoveButton );
		if ( Controller.RunByDefault ) isRunning = !isRunning;

		float wishSpeed = isRunning ? Controller.RunSpeed : Controller.WalkSpeed;
		if ( Controller.IsDucking ) wishSpeed = Controller.DuckedSpeed;

		if ( wishDir.IsNearlyZero( 0.1f ) )
			return Vector3.Zero;

		// Just return the desired velocity — no smoothing here.
		return wishDir * wishSpeed;
	}

	public override void AddVelocity()
	{
		var body = Controller.Body;
		Vector3 vel = body.Velocity;

		// Friction always (this is what was missing before)
		vel = vel.WithFriction( Friction * Time.Delta, StopSpeed );

		// Accelerate toward wish only when there's input
		Vector3 wish = Controller.WishVelocity;
		if ( !wish.IsNearZeroLength )
			vel = vel.WithAcceleration( wish, Acceleration * Time.Delta );

		if ( vel.IsNearlyZero( 0.01f ) )
			vel = Vector3.Zero;

		body.Velocity = vel;
	}
}
