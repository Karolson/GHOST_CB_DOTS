using System;
using System.Collections.Generic;
using System.Linq;
using System.Net.Sockets;
using System.Text;
using System.Threading;

namespace TCP_READ
{
    class Program
    {
        static void Main(string[] args)
        {
            TcpClient tcpClient = new TcpClient("192.168.1.7", 6151);

            while (!tcpClient.Connected)
            {
                Thread.Sleep(100);
            }

            Console.WriteLine("Connected");

            tcpClient.NoDelay = true;
            NetworkStream stream = tcpClient.GetStream();

            // Translate the passed message into ASCII and store it as a Byte array.
            Byte[] data = { 0xff, 0xff };

            // Send the message to the connected TcpServer.
            // stream.Write(data, 0, data.Length);

            //Console.WriteLine("Sent: {0}", "HELLO");

            int b = 1;

            while (true)
            {
                try
                {
                    String responseData = String.Empty;
                    data = new Byte[148];
                    Int32 bytes;
                    // Read the first batch of the TcpServer response bytes.

                    bytes = stream.Read(data, 0, 148);


                    responseData = System.Text.Encoding.ASCII.GetString(data, 0, bytes);
                    //if(data[0] != 0)

                    Console.ForegroundColor = PickColor(b % 4);
                    Console.Write("Received: \n{0} \nHex: \n{1}", new string(responseData.Where(c => !char.IsControl(c)).ToArray()), BitConverter.ToString(data));
                    Console.ResetColor();
                    Console.WriteLine();

                    b++;
                    //Console.WriteLine("Received: \n{0}", responseData);

                    //data = Encoding.ASCII.GetBytes("KEEPALIVE");

                    // Send the message to the connected TcpServer.
                    //stream.Write(data, 0, data.Length);

                    Thread.Sleep(300);
                }
                catch(Exception ex)
                {
                    Console.WriteLine(ex);
                    while (!tcpClient.Connected)
                    {
                        Console.WriteLine("reconnect...");
                        try {
                            tcpClient.Close(); ;
                            tcpClient = new TcpClient("192.168.1.7", 6151);
                            stream = tcpClient.GetStream();
                            stream.Flush();
                        }
                        catch (Exception ex2) { Console.WriteLine(ex2); }
                        Thread.Sleep(100);
                    }
                    Console.WriteLine("reconnected!");
                }
            }
            //Console.ReadKey();
        }

         static System.ConsoleColor PickColor(int b)
        {
            switch (b)
            {
                case 1: return System.ConsoleColor.Red;
                case 2: return System.ConsoleColor.Blue;
                case 3: return System.ConsoleColor.Green;
                case 4: return System.ConsoleColor.Yellow;
            }
            return System.ConsoleColor.White;
        }
    }
    
}
