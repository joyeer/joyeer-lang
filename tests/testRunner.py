import unittest
import sys
import subprocess
from unittest import result

joyeerSourceCode = ""
joyeerInterpreter = ""  
class JoyeerTestCase(unittest.TestCase):
    def runTest(self):
        sourceCodeResult = joyeerSourceCode + ".result.txt"
        result = subprocess.run([joyeerInterpreter, joyeerSourceCode], stdout=subprocess.PIPE).stdout.decode('utf-8')
        result = "\n".join(result.splitlines())
        f = open(sourceCodeResult, 'r')
        exceptedResult = "\n".join(f.read().splitlines())

        f.close()

        print("##########")
        print(result)
        print("##########")
        print(exceptedResult)
        self.assertEqual(result, exceptedResult)

if __name__ == '__main__':
    joyeerSourceCode = sys.argv.pop()
    joyeerInterpreter = sys.argv.pop()
    unittest.main()
    