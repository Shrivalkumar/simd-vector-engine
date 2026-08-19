import type { Metadata } from "next";
import { Geist, Geist_Mono } from "next/font/google";
import "./globals.css";

const geistSans = Geist({
  variable: "--font-geist-sans",
  subsets: ["latin"],
});

const geistMono = Geist_Mono({
  variable: "--font-geist-mono",
  subsets: ["latin"],
});

export const metadata: Metadata = {
  title: "VectorDB Control Plane",
  description: "Live topology and search telemetry for VectorDB.",
  applicationName: "VectorDB",
  openGraph: {
    title: "VectorDB Control Plane",
    description: "Distributed vector search, instrumented in real time.",
    images: ["/og.png"],
  },
  twitter: {
    card: "summary_large_image",
    title: "VectorDB Control Plane",
    description: "Distributed vector search, instrumented in real time.",
    images: ["/og.png"],
  },
  icons: {
    icon: "/favicon.svg",
    shortcut: "/favicon.svg",
  },
};

export default function RootLayout({
  children,
}: Readonly<{
  children: React.ReactNode;
}>) {
  return (
    <html lang="en">
      <body
        className={`${geistSans.variable} ${geistMono.variable} antialiased`}
      >
        {children}
      </body>
    </html>
  );
}
