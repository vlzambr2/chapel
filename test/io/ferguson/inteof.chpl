use FileSystem;
use IO;

config const filename = "inteof-test.nums";
config const verbose = false;
const sizes = ( 1, 100, 1000, 10000, 100000, 1000000 );

for i in 0..sizes.size-1
{
  var n = sizes[i];
  for reopen in 1..1 // TODO 0..1
  {
    if verbose then writeln("n=", n, " reopen=", reopen, " read text");
    {
      var infile = open(filename, ioMode.cwr);
      var writer = infile.writer();

      writer.writeln(n);
      for i in 1..n {
        writer.writeln(i);
      }

      writer.close();

      if reopen {
        infile.close();
        infile = open(filename, ioMode.r);
      }

      var reader = infile.reader();

      var gotn:int;
      reader.read(gotn);

      for i in 1..gotn {
        var x:int;
        reader.read(x);
      }

      var badint: int;
      var dataRemains: bool = false;
      try! {
        dataRemains = reader.read(badint);
      } catch e: SystemError {
        dataRemains = true;
      }
      if (dataRemains) {
        halt("Data remains at end of file");
      }

      infile.close();
    }
    if verbose then writeln("n=", n, " reopen=", reopen, " readln text");
    {
      var infile = open(filename, ioMode.cwr);
      var writer = infile.writer();

      writer.writeln(n);
      for i in 1..n {
        writer.writeln(i);
      }

      writer.close();

      if reopen {
        infile.close();
        infile = open(filename, ioMode.r);
      }

      var reader = infile.reader();

      var gotn:int;
      reader.readln(gotn);

      for i in 1..gotn {
        var x:int;
        reader.readln(x);
      }

      var badint: int;
      var dataRemains: bool = false;
      try! {
        dataRemains = reader.read(badint);
      } catch e: SystemError {
        dataRemains = true;
      }
      if (dataRemains) {
        halt("Data remains at end of file");
      }

      infile.close();
    }
    if verbose then writeln("n=", n, " reopen=", reopen, " read binary");
    {
      var infile = open(filename, ioMode.cwr);
      var writer = infile.writer(serializer=new binarySerializer());

      writer.write(n);
      for i in 1..n {
        writer.write(i);
      }

      writer.close();

      if reopen {
        infile.close();
        infile = open(filename, ioMode.r);
      }

      var reader = infile.reader(deserializer=new binaryDeserializer());

      var gotn:int;
      reader.read(gotn);

      for i in 1..gotn {
        var x:int;
        reader.read(x);
      }

      var badint: int;
      var dataRemains: bool = false;
      try! {
        dataRemains = reader.read(badint);
      } catch e: SystemError {
        dataRemains = true;
      }
      if (dataRemains) {
        halt("Data remains at end of file");
      }

      infile.close();
    }
  }
}

FileSystem.remove(filename);
