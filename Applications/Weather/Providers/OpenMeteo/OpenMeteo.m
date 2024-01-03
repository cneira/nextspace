/*
  Class:               OpenMeteo
  Class descritopn:    Get and parse weather condition and forecast 
                       from yahoo.com.

  Copyright (C) 2016 Sergii Stoian <stoian255@gmail.com>

  -query: method 
  Created by Guilherme Chapiewski on 10/19/12.
  Copyright (c) 2012 Guilherme Chapiewski. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include <AppKit/NSImage.h>
#include <WeatherProvider.h>

#import "OpenMeteo.h"

#define QUERY_PREFIX @"https://api.open-meteo.com/v1/forecast?timezone=auto"
// Options
/*
   latitude=
   longitude=
   current=temperature_2m,apparent_temperature,is_day,weather_code,cloud_cover,wind_speed_10m
   timezone=auto
 */
#define QUERY_CURRENT                                                                            \
  @"current=temperature_2m,apparent_temperature,is_day,weather_code,cloud_cover,wind_speed_10m," \
  @"relative_humidity_2m"

#define QUERY_DAILY                                                                       \
  @"daily=temperature_2m_min,temperature_2m_max,weather_code,cloud_cover,wind_speed_10m," \
  @"relative_humidity_2m "

@implementation OpenMeteo

//
// Weather Provider protocol
//
@synthesize locationName;
@synthesize current;
@synthesize forecast;
- (NSString *)name
{
  return @"OpenMeteo";
}

- (NSDictionary *)_queryLocationByName:(NSString *)name
{
  NSString *geoQuery;

  geoQuery = [NSString stringWithFormat:@"https://geocoding-api.open-meteo.com/v1/"
                                        @"search?name=%@&count=10&language=en&format=json",
                                        name];
  return [self query:geoQuery];  
}

- (NSArray *)locationsListForName:(NSString *)name
{
  NSDictionary *geoResults = [self _queryLocationByName:name];
  NSMutableArray *cityList = [NSMutableArray new];

  if (geoResults != nil) {
    NSArray *resultsList = geoResults[@"results"];
    if (resultsList && [resultsList count] > 0) {
      for (NSDictionary *entry in resultsList) {
        [cityList addObject:entry[@"name"]];
      }
    }
  }
  return cityList;
}

- (BOOL)setLocationByName:(NSString *)name
{
  NSDictionary *geoResults = [self _queryLocationByName:name];
  NSArray *resultsList;

  if (geoResults != nil) {
    resultsList = geoResults[@"results"];
    if (resultsList && [resultsList count] > 0) {
      for (NSDictionary *entry in resultsList) {
        if ([entry[@"name"] isEqualToString:name]) {
          // NSLog(@"Got coordinates for %@: lat:%@ long: %@", name, entry[@"latitude"],
          //       entry[@"longitude"]);
          if (latitude) {
            [latitude release];
          }
          latitude = [entry[@"latitude"] copy];
          if (longtitude) {
            [longtitude release];
          }
          longtitude = [entry[@"longitude"] copy];
          locationName = [name copy];
          return YES;
        }
      }
    }
  }

  return NO;
}

- (NSDictionary *)temperatureUnitsList {
  return @{@"Celsisus" : @"celsius", @"Farenheit" : @"farenheit"};
}

- (void)setTemperatureUnits:(NSString *)name
{
  //
}

//
// Initalization
//
- (id)init
{
  forecast = [[WeatherForecast alloc] init];
  current = [[WeatherCurrent alloc] init];
  
  return self;
}

- (void)dealloc
{
  [current release];
  [forecast release];
  TEST_RELEASE(latitude);
  TEST_RELEASE(longtitude);
  
  [super dealloc];
}

// - (void)appendForecastForDay:(NSString *)day
//                     highTemp:(NSString *)high
//                      lowTemp:(NSString *)low
//                  description:(NSString *)desc
// {
//   NSDictionary *dayForecast;

//   dayForecast = @{@"Day" : day, @"High" : high, @"Low" : low, @"Description" : desc};
//   // NSLog(@"Append forecast: %@", dayForecast);
//   [forecastList addObject:dayForecast];
// }

/*
   WMO Weather interpretation codes (WW)
   Code        Description
   ~~~~        ~~~~~~~~~~~
   0           Clear sky
   1           Mainly clear
   2           Partly cloudy
   3           Overcast
   
   45, 48      Fog and depositing rime fog
   
   51, 53, 55  Drizzle: Light, moderate, and dense intensity
   61, 63, 65  Rain: Slight, moderate and heavy intensity
   80, 81, 82  Rain showers: Slight, moderate, and violent
   56, 57      Freezing Drizzle: Light and dense intensity
   66, 67 	    Freezing Rain: Light and heavy intensity
   
   71, 73, 75 	Snow fall: Slight, moderate, and heavy intensity
   77 	        Snow grains
   85, 86      Snow showers slight and heavy
   
   95 * 	      Thunderstorm: Slight or moderate
   96, 99 * 	  Thunderstorm with slight and heavy hail
*/

- (BOOL)fetchWeather
{
  NSString *queryString;
  NSDictionary *results;
  NSString *imageName;
  NSString *imagePostfix;
  NSImage *image;
  NSString *imagePath;
  int weather_code;

  queryString = [NSString stringWithFormat:@"%@&latitude=%@&longitude=%@&%@",
                                           QUERY_PREFIX,
                                           latitude, longtitude,
                                           QUERY_CURRENT];
  // NSLog(@"Query string: %@", queryString);
  results = [self query:queryString];
  if (results != nil && results[@"error"] == nil &&
      [results[@"current"] isKindOfClass:[NSNull class]] == NO) {
    [current release];
    current = [[WeatherCurrent alloc] init];
    // NSLog(@"Temperature is a %@", results[@"current"][@"temperature_2m"]);
    float temp = [results[@"current"][@"temperature_2m"] floatValue];
    current.temperature = [NSString stringWithFormat:@"%.0f", roundf(temp)];
    current.humidity = [results[@"current"][@"relative_humidity_2m"] copy];

    imagePostfix = [results[@"current"][@"is_day"] intValue] == 1 ? @"d" : @"n";
    weather_code = [results[@"current"][@"weather_code"] intValue];
    switch (weather_code) {
      case 0:
      case 1:
      case 2:
      case 3:
        imageName = [NSString stringWithFormat:@"0%i%@", weather_code, imagePostfix];
        break;
      case 45:
      case 48:
      // case 51:
      // case 53:
      // case 55:
      // case 56:
      // case 57:
      case 71:
      case 72:
      case 73:
      case 77:
        imageName = [NSString stringWithFormat:@"%i%@", weather_code, imagePostfix];
        break;
      default:
        imageName = @"na";
    }
    imagePath = [[NSBundle bundleForClass:self.class] pathForResource:imageName ofType:@"png"];
    image = [[NSImage alloc] initWithContentsOfFile:imagePath];
    if (image == nil) {
      imagePath = [[NSBundle bundleForClass:self.class] pathForResource:@"na" ofType:@"png"];
      image = [[NSImage alloc] initWithContentsOfFile:imagePath];
    }
    current.image = [image copy];
    [image release];
    return YES;
  } else if (results[@"error"] != nil) {
    current.error = results[@"reason"];
  } else {
    current.error = @"Unknown error occured while fetching weather data!";
  }

  // for (NSDictionary *forecast in channel[@"item"][@"forecast"]) {
  //   [self appendForecastForDay:forecast[@"day"]
  //                     highTemp:forecast[@"high"]
  //                      lowTemp:forecast[@"low"]
  //                  description:forecast[@"desc"]];
  // }
  // [weatherCondition setObject:[NSDate date] forKey:@"Fetched"];

  return NO;
}

- (NSDictionary *)query:(NSString *)statement
{
  NSData *jsonData;
  NSError *error = nil;
  NSDictionary *results = nil;

  if ((jsonData = [[NSURL URLWithString:statement] resourceDataUsingCache:NO])) {
    NSLog(@"Got %lu bytes with query: %@", [jsonData length], statement);
    results = [NSJSONSerialization JSONObjectWithData:jsonData options:0 error:&error];
    if (error || !results) {
      NSLog(@"[%@ %@] JSON error: %@", NSStringFromClass([self class]), NSStringFromSelector(_cmd),
            error.localizedDescription);
      results = nil;
    }
  }

  return results;
}

@end

