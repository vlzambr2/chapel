use IO;

var filename = "shortfile.txt";
var f = open(filename, ioMode.r);
var chnl = f.reader(deserializer=new binaryDeserializer());
// If I remove kind=ionative, the program completes correctly

var line: string;
var lineSizeLimit = 1024;
while (chnl.readString(line, lineSizeLimit)) {
  write(line);
}
f.close();
