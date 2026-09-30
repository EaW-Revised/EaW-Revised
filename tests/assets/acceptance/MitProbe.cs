using PG.StarWarsGame.LSP.Assets.Models;

if (args.Length != 2)
{
    Console.Error.WriteLine("usage: MitProbe <model.alo> <animation.ala>");
    return 2;
}

try
{
    var model = AloModelReader.Read(File.ReadAllBytes(args[0]));
    var vertices = model.Meshes.SelectMany(m => m.SubMeshes).Sum(s => s.Vertices.Count);
    var indices = model.Meshes.SelectMany(m => m.SubMeshes).Sum(s => s.Indices.Count);
    var parameters = model.Meshes.SelectMany(m => m.SubMeshes).Sum(s => s.Parameters.Count);
    Console.WriteLine($"model bones={model.Bones.Count} meshes={model.Meshes.Count} lights={model.Lights.Count} proxies={model.Proxies.Count} dazzles={model.Dazzles.Count} vertices={vertices} indices={indices} parameters={parameters}");

    var animation = AlaAnimationReader.Read(File.ReadAllBytes(args[1]));
    var samples = animation.Bones.Sum(b => b.Frames.Count);
    Console.WriteLine($"animation version={animation.FormatVersion} frames={animation.FrameCount} tracks={animation.Bones.Count} samples={samples} duration={animation.Duration:R}");
    return 0;
}
catch (Exception exception) when (exception is AloFormatException || exception is EndOfStreamException || exception is ArgumentException)
{
    Console.Error.WriteLine($"MIT parser rejected input: {exception.GetType().Name}");
    return 1;
}
